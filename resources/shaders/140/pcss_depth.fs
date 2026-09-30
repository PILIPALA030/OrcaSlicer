#version 140
void main()
{
    // User clipping occurs before rasterization through gl_ClipDistance. No fragment discard or
    // custom depth write: keep the depth-only pass eligible for the driver's early-depth fast path.
}
