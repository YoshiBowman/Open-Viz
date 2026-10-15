// Included before a source file from core/ is pulled into this module. Unreal builds its own
// modules with warnings as errors; the core and its third-party libraries are held to the
// warning set in core/CMakeLists.txt instead.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
