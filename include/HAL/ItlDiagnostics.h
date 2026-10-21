#ifndef ITL_DIAGNOSTICS_H
#define ITL_DIAGNOSTICS_H

// The diagnostics are compiled in by default, so a build that says nothing
// about them behaves like the one this tree was developed with. A release
// build passes -DSEQUOIA_DIAGNOSTICS=0 and gets none of them: no
// properties in the registry, no log output, no stage markers.
//
// The switch has to be usable below the header's own layer, so the file
// that defines the log macro repeats the default rather than including
// this one.
#ifndef SEQUOIA_DIAGNOSTICS
#define SEQUOIA_DIAGNOSTICS 1
#endif

// The diagnostics are published as properties on the provider. A release
// build drops the calls whole: the arguments are not evaluated either, so
// nothing that would have been computed for them happens.
#if SEQUOIA_DIAGNOSTICS
#define ITLWM_PUBLISH_DIAGNOSTIC(target, ...) (target)->setProperty(__VA_ARGS__)
#define ITLWM_LOG(...) IOLog(__VA_ARGS__)
#else
#define ITLWM_PUBLISH_DIAGNOSTIC(target, ...) do { } while (0)
#define ITLWM_LOG(...) do { } while (0)
#endif

#endif
