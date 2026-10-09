#ifndef slic3r_ShadowMeshProxy_hpp_
#define slic3r_ShadowMeshProxy_hpp_

#include "GLModel.hpp"
#include "libslic3r/QuadricEdgeCollapse.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <wx/timer.h>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

// Only the depth pass uses this proxy. The editable mesh, picking mesh and
// visible LODs are untouched. One proxy is owned by each shared source mesh.
class ShadowMeshProxy : private wxTimer
{
public:
    static constexpr uint32_t TRIANGLE_BUDGET = 100000;

    ShadowMeshProxy(std::function<bool()> enabled, std::function<bool()> can_start, std::function<void()> redraw)
        : m_enabled(std::move(enabled)), m_can_start(std::move(can_start)), m_redraw(std::move(redraw)) {}
    ~ShadowMeshProxy() override
    {
        Stop();
        if (m_task)
            m_task->cancelled.store(true, std::memory_order_relaxed);
    }
    ShadowMeshProxy(const ShadowMeshProxy&) = delete;
    ShadowMeshProxy& operator=(const ShadowMeshProxy&) = delete;

    // Main/GL thread only. No mesh-sized copying or QEM runs on this thread.
    GLModel* get(const std::shared_ptr<const TriangleMesh>& source)
    {
        if (m_model)
            return m_model.get();
        m_source = source;
        if (!m_task)
            m_task = std::make_shared<Task>();
        const State state = m_task->state.load(std::memory_order_acquire);
        if (state == State::Ready) {
            Stop();
            auto mesh = std::move(m_task->result);
            if (mesh && !mesh->indices.empty() && mesh->indices.size() <= TRIANGLE_BUDGET) {
                // Position-only depth geometry: no normal/color splitting.
                // Expand only the bounded result, never the original mesh.
                GLModel::Geometry geometry;
                geometry.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3};
                geometry.reserve_vertices(3 * mesh->indices.size());
                geometry.reserve_indices(3 * mesh->indices.size());
                unsigned int index = 0;
                for (const auto& face : mesh->indices) {
                    for (unsigned int corner = 0; corner < 3; ++corner)
                        geometry.add_vertex(mesh->vertices[face[corner]]);
                    geometry.add_triangle(index, index + 1, index + 2);
                    index += 3;
                }
                m_model = std::make_unique<GLModel>();
                m_model->init_from(std::move(geometry));
                BOOST_LOG_TRIVIAL(info) << "PCSS shadow proxy ready: " << source->its.indices.size()
                                        << " -> " << mesh->indices.size() << " triangles";
            }
            m_task->state.store(State::Consumed, std::memory_order_release);
            m_source.reset();
            return m_model.get();
        }
        if (state == State::Cancelled || (state == State::Idle && m_task->cancelled.load(std::memory_order_relaxed))) {
            m_task = std::make_shared<Task>();
        } else if (state == State::Failed || state == State::Consumed) {
            return nullptr;
        }
        try_start();
        if (!IsRunning())
            Start(100); // Poll completion on the UI thread, not at render FPS.
        return nullptr;
    }

private:
    enum class State { Idle, Working, Ready, Failed, Cancelled, Consumed };
    struct Cancelled {};
    struct Task {
        std::atomic<State> state{State::Idle};
        std::atomic<bool> cancelled{false};
        std::unique_ptr<indexed_triangle_set> result;
        std::string error;
        // Deliberately NO GLModel or GUI pointers in the worker-owned state.
    };
    std::shared_ptr<const TriangleMesh> m_source;
    std::shared_ptr<Task> m_task;
    std::unique_ptr<GLModel> m_model;
    std::function<bool()> m_enabled;
    std::function<bool()> m_can_start;
    std::function<void()> m_redraw;

    // Shared ownership also keeps the slot alive during process teardown.
    static std::shared_ptr<std::atomic<bool>> worker_slot()
    {
        static auto slot = std::make_shared<std::atomic<bool>>(false);
        return slot;
    }

    void try_start()
    {
        if (!m_source || !m_task || !m_can_start() || m_task->state.load(std::memory_order_acquire) != State::Idle)
            return;
        auto slot = worker_slot();
        bool expected = false;
        if (!slot->compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            return; // At most one extra QEM job, even with many unique meshes.
        const auto source = m_source;
        const auto task = m_task;
        task->state.store(State::Working, std::memory_order_release);
        try {
            std::thread([source, task, slot] {
                auto check_cancel = [task] {
                    if (task->cancelled.load(std::memory_order_relaxed))
                        throw Cancelled{};
                };
                try {
                    check_cancel();
                    auto mesh = std::make_unique<indexed_triangle_set>(source->its);
                    check_cancel();
                    its_quadric_edge_collapse(*mesh, TRIANGLE_BUDGET, nullptr, check_cancel);
                    check_cancel();
                    if (mesh->indices.empty() || mesh->indices.size() > TRIANGLE_BUDGET)
                        throw std::runtime_error("simplifier did not produce a bounded nonempty mesh");
                    task->result = std::move(mesh);
                    // Last access to result before the GUI thread may consume it.
                    task->state.store(State::Ready, std::memory_order_release);
                } catch (const Cancelled&) {
                    task->state.store(State::Cancelled, std::memory_order_release);
                } catch (const std::exception& error) {
                    task->error = error.what();
                    task->state.store(State::Failed, std::memory_order_release);
                } catch (...) {
                    task->error = "unknown simplification failure";
                    task->state.store(State::Failed, std::memory_order_release);
                }
                slot->store(false, std::memory_order_release);
            }).detach();
        } catch (const std::exception& error) {
            task->error = error.what();
            task->state.store(State::Failed, std::memory_order_release);
            slot->store(false, std::memory_order_release);
        }
    }

    void Notify() override
    {
        if (!m_enabled()) {
            if (m_task)
                m_task->cancelled.store(true, std::memory_order_relaxed);
            Stop();
            return;
        }
        const State state = m_task->state.load(std::memory_order_acquire);
        // Give render() a chance to adopt an existing completed LOD before
        // launching another mesh-sized QEM job. A busy slot is also retried
        // via the normal render path, never by spinning on the UI thread.
        if (state == State::Idle && m_can_start() && !worker_slot()->load(std::memory_order_acquire)) {
            Stop();
            m_redraw();
            return;
        }
        if (state == State::Consumed) {
            Stop();
            return;
        }
        if (state == State::Ready || state == State::Failed || state == State::Cancelled) {
            Stop();
            if (state == State::Failed)
                BOOST_LOG_TRIVIAL(warning) << "PCSS shadow proxy unavailable: " << m_task->error;
            m_redraw();
        }
    }
};

}} // namespace Slic3r::GUI
#endif
