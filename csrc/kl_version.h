/* kl_version.h -- the version of Klattsch Native: the C engine, the text
 * front end, the generator and (later) the NVDA add-on, as one product.
 *
 * The one place the number is written. CMakeLists.txt reads it into
 * project(VERSION), the generator's VERSIONINFO and window title come from
 * that, and packaging/make-dist.py reads it for the archive name -- so they
 * cannot drift (docs/ROADMAP.md, "One version constant").
 *
 * Not upstream's version. package.json's 0.8.0 is Tony Gies's JavaScript
 * package, kept frozen as the reference; this numbers the native work built
 * on it, which starts its own count. 0.5.0 is the first beta, 2026-09-25.
 *
 * Part of this repository's MIT-licensed work; klattsch is Tony Gies's, see
 * LICENSE and NOTICE.md.
 */
#ifndef KL_VERSION_H
#define KL_VERSION_H

#define KL_VERSION_MAJOR 0
#define KL_VERSION_MINOR 5
#define KL_VERSION_PATCH 0
#define KL_VERSION_STRING "0.5.0"

/* What the build is for. "beta" goes to testers, not to a release. */
#define KL_VERSION_STAGE "beta"

/* Both together, spelled out for places that cannot join strings (the
 * resource compiler). Keep in step with the two above. */
#define KL_VERSION_DISPLAY "0.5.0 beta"

#endif /* KL_VERSION_H */
