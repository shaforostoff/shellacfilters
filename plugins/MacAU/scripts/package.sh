#!/bin/sh
#
# Builds one macOS installer package that installs all three ShellacFilters
# Audio Units.
#
#     plugins/dist/au/mac/ShellacFilters-AudioUnits-<version>.pkg
#
# Double-clickable, and it puts Declick, Dehum and ParaEQ where every Audio Unit
# host on the machine will find them.
#
# Why this is shorter than its VirtualDJ counterpart. An Audio Unit has one
# place to live and has had since 10.0 - ~/Library/Audio/Plug-Ins/Components for
# one user, /Library/... for everyone - so the payload lands where it belongs
# and there is no postinstall fan-out to write. The package installs into the
# user's home, which is where a plug-in you installed for yourself goes and why
# it asks for no administrator password.
#
# The one thing the postinstall does do is restart AudioComponentRegistrar.
# That daemon caches what it found last time it scanned, so without the nudge a
# host started straight after the install can still be working from the old
# list. It is started again on demand, so killing it costs nothing.
#
# What is NOT here, and where it went: there is no Rez step and no .r resource
# in the bundles. Component Manager registration has been dead since 10.7 and
# building one needs AUResources.r from the CoreAudio SDK that is not in this
# tree. Registration is the AudioComponents array in each Info.plist instead -
# see scripts/build.sh, which is also what answers "is MacSignedAU any help
# here": the useful half of MacSignedAU is exactly that plist change and the
# AUDIOCOMPONENT_ENTRY that goes with it, and both are in the sources now. The
# other half of MacSignedAU is a Header Search Path pointing at a CoreAudio SDK
# on somebody else's desktop, which is the problem rather than the answer.
#
# Signing. --sign takes a "Developer ID Installer" identity, which productbuild
# signs the finished package with. --codesign takes a "Developer ID
# Application" identity for the bundles inside; pass "-" for ad-hoc. Note that
# some signature is not optional on Apple Silicon the way it is for a VST on
# Intel: an unsigned component will not load at all, so build.sh ad-hoc signs by
# default and this re-signs whatever it is given here.
#
# Notarising. --notarize submits the finished package to Apple, waits for the
# answer, staples the ticket to it and checks the result with spctl. It needs
# both identities, because Apple rejects a package whose nested code is not
# Developer ID signed with the hardened runtime and a secure timestamp - so
# with --notarize the codesign step is strict rather than best-effort, and an
# ad-hoc "-" is refused up front rather than 20 minutes later by the notary.
#
# Credentials come one of three ways. In order of preference:
#
#   1. A keychain profile, stored once and then named by --notary-profile:
#
#        xcrun notarytool store-credentials ShellacFilters \
#              --apple-id you@example.com \
#              --team-id ABCDE12345 \
#              --password abcd-efgh-ijkl-mnop
#
#        scripts/package.sh --sign ... --codesign ... \
#              --notarize --notary-profile ShellacFilters
#
#      The app-specific password is typed once, into a tool that puts it in the
#      keychain, and never appears again - not in argv, not in the history file,
#      not in a CI log.
#
#   2. An App Store Connect API key, which is what a shared build machine
#      should use because it is not tied to anyone's Apple ID and can be
#      revoked on its own:
#
#        NOTARY_KEY=/path/AuthKey_XXXX.p8 \
#        NOTARY_KEY_ID=XXXXXXXXXX \
#        NOTARY_ISSUER=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee \
#            scripts/package.sh ... --notarize
#
#   3. Apple ID, team ID and an app-specific password, for a one-off:
#
#        NOTARY_PASSWORD=abcd-efgh-ijkl-mnop \
#            scripts/package.sh ... --notarize \
#                --apple-id you@example.com --team-id ABCDE12345
#
#      The Apple ID and the team ID are options because neither is a secret.
#      The password is not: an option value is in this shell's history file and
#      visible to every process on the machine for as long as the script runs,
#      so it is read from NOTARY_PASSWORD, or from whatever --password-env
#      names. Note that notarytool itself takes the password in argv, so this
#      narrows the window to that one command rather than closing it - which is
#      the argument for (1) and (2) over (3).
#
#      The password is the app-specific one from appleid.apple.com, not the
#      Apple ID password, and the team ID is the ten characters in the
#      parentheses at the end of the signing identity.
#
# Usage:
#   scripts/package.sh                        build, verify, package
#   scripts/package.sh --skip-build           package whatever is in dist already
#   scripts/package.sh --skip-tests           forwarded to build.sh
#   scripts/package.sh --version 1.0.1        override the version in the sources
#   scripts/package.sh --codesign -           ad-hoc sign the bundles first
#   scripts/package.sh --sign "Developer ID Installer: Some One (TEAMID)"
#   scripts/package.sh --notarize --notary-profile NAME
#   scripts/package.sh --notarize --apple-id you@example.com --team-id ABCDE12345
#   scripts/package.sh --password-env MY_VAR  read the password from MY_VAR
#   scripts/package.sh --notary-timeout 45m   how long to wait for Apple
#   scripts/package.sh --out DIR              somewhere other than dist/au/mac
#
# Environment:
#   OUTDIR          where the built components are read from. plugins/dist/au/mac.
#   PKGOUT          where the .pkg is written. Same, unless --out says otherwise.
#   NOTARY_PROFILE  keychain profile name, as --notary-profile
#   NOTARY_APPLE_ID, NOTARY_TEAM_ID, NOTARY_PASSWORD
#   NOTARY_KEY, NOTARY_KEY_ID, NOTARY_ISSUER    App Store Connect API key

set -eu
export LC_ALL=C

case "$(uname -s)" in
    Darwin) ;;
    *) echo "package.sh builds a macOS .pkg and needs pkgbuild/productbuild." >&2
       echo "There is no Audio Unit to package anywhere else." >&2
       exit 1 ;;
esac

root=$(cd "$(dirname "$0")/.." && pwd)      # plugins/MacAU
plugins=$(cd "$root/.." && pwd)             # plugins
repo=$(cd "$plugins/.." && pwd)             # the tree
src="${OUTDIR:-$plugins/dist/au/mac}"
out="${PKGOUT:-$plugins/dist/au/mac}"
stage="$root/build/pkg"

plugin_names="Declick Dehum ParaEQ"

identifier="com.shellacfilters.audiounits"
skip_build=0
skip_tests=0
version=""
sign=""
codesign_id=""

notarize=0
notary_profile="${NOTARY_PROFILE:-}"
notary_apple_id="${NOTARY_APPLE_ID:-}"
notary_team_id="${NOTARY_TEAM_ID:-}"
notary_key="${NOTARY_KEY:-}"
notary_key_id="${NOTARY_KEY_ID:-}"
notary_issuer="${NOTARY_ISSUER:-}"
password_env="NOTARY_PASSWORD"
notary_timeout="30m"

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-build) skip_build=1 ;;
        --skip-tests) skip_tests=1 ;;
        --version)    shift; version="${1:-}" ;;
        --sign)       shift; sign="${1:-}" ;;
        --codesign)   shift; codesign_id="${1:-}" ;;
        --out)        shift; out="${1:-}" ;;
        --identifier) shift; identifier="${1:-}" ;;
        --notarize)   notarize=1 ;;
        --notary-profile) shift; notary_profile="${1:-}" ;;
        --apple-id)   shift; notary_apple_id="${1:-}" ;;
        --team-id)    shift; notary_team_id="${1:-}" ;;
        --password-env) shift; password_env="${1:-}" ;;
        --notary-timeout) shift; notary_timeout="${1:-}" ;;
        --password|--notary-password)
            # Deliberately not accepted. See the header: an option value is in
            # the history file and in ps output. NOTARY_PASSWORD, or a keychain
            # profile, which is better than both.
            echo "$1 is not an option - the password would land in your shell" >&2
            echo "history and in ps output. Pass it in the environment:" >&2
            echo "  NOTARY_PASSWORD=abcd-efgh-ijkl-mnop $0 ... --notarize" >&2
            echo "or store it once and name the profile:" >&2
            echo "  xcrun notarytool store-credentials NAME --apple-id ... \\" >&2
            echo "        --team-id ... --password ..." >&2
            echo "  $0 ... --notarize --notary-profile NAME" >&2
            exit 2 ;;
        -h|--help)
            awk 'NR > 1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"
            exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

for tool in pkgbuild productbuild ditto; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool was not found on PATH. Install the Xcode command line tools." >&2
        exit 1
    }
done

# --- notarisation preflight -------------------------------------------------
#
# All of this is checked before the build rather than after it. Notarising is
# the last step of a run that compiles three plug-ins and Apple's SDK and runs
# three verify suites, and finding out then that a password was never exported
# is a wasted ten minutes.

notary_password=""
if [ "$notarize" -eq 1 ]; then
    command -v xcrun >/dev/null 2>&1 || {
        echo "xcrun was not found; notarising needs the Xcode command line tools." >&2
        exit 1
    }
    xcrun --find notarytool >/dev/null 2>&1 || {
        echo "notarytool was not found. It needs Xcode 13 or newer;" >&2
        echo "altool's notarisation service was switched off in 2023." >&2
        exit 1
    }

    [ -n "$sign" ] || {
        echo "--notarize needs --sign \"Developer ID Installer: ...\": Apple will" >&2
        echo "not notarise an unsigned package." >&2
        exit 2
    }
    [ -n "$codesign_id" ] || {
        echo "--notarize needs --codesign \"Developer ID Application: ...\" as well:" >&2
        echo "the components inside the package are nested code and are checked too." >&2
        exit 2
    }
    [ "$codesign_id" = "-" ] && {
        echo "--codesign - is ad-hoc, which Apple will reject. Notarising needs a" >&2
        echo "Developer ID Application identity. security find-identity -v" >&2
        echo "-p codesigning lists what this machine has." >&2
        exit 2
    }

    if [ -n "$notary_profile" ]; then
        creds="keychain profile $notary_profile"
    elif [ -n "$notary_key" ]; then
        [ -f "$notary_key" ] || {
            echo "NOTARY_KEY does not name a file: $notary_key" >&2
            exit 2
        }
        [ -n "$notary_key_id" ] && [ -n "$notary_issuer" ] || {
            echo "an App Store Connect key needs NOTARY_KEY_ID and NOTARY_ISSUER too." >&2
            exit 2
        }
        creds="API key $notary_key_id"
    else
        # eval rather than ${!password_env}, which is a bashism and this is sh.
        notary_password=$(eval "printf '%s' \"\${$password_env:-}\"")
        if [ -z "$notary_apple_id" ] || [ -z "$notary_team_id" ] || [ -z "$notary_password" ]; then
            echo "--notarize needs credentials. Either store them once:" >&2
            echo >&2
            echo "  xcrun notarytool store-credentials ShellacFilters \\" >&2
            echo "        --apple-id you@example.com --team-id ABCDE12345 \\" >&2
            echo "        --password abcd-efgh-ijkl-mnop" >&2
            echo "  $0 ... --notarize --notary-profile ShellacFilters" >&2
            echo >&2
            echo "or pass them for this run, with the app-specific password in" >&2
            echo "the environment rather than in an option:" >&2
            echo >&2
            echo "  $password_env=abcd-efgh-ijkl-mnop \\" >&2
            echo "      $0 ... --notarize --apple-id you@example.com --team-id ABCDE12345" >&2
            echo >&2
            [ -z "$notary_apple_id" ] && echo "  missing: --apple-id" >&2
            [ -z "$notary_team_id" ]  && echo "  missing: --team-id" >&2
            [ -z "$notary_password" ] && echo "  missing: \$$password_env in the environment" >&2
            exit 2
        fi
        creds="$notary_apple_id, team $notary_team_id"
    fi
fi

# One place that knows how the credentials were given, so that submit and log
# do not each have to. The password reaches notarytool in argv - there is no
# other way to give it one - which is the argument for a keychain profile.
notarytool_run() {
    action=$1
    shift
    if [ -n "$notary_profile" ]; then
        xcrun notarytool "$action" "$@" --keychain-profile "$notary_profile"
    elif [ -n "$notary_key" ]; then
        xcrun notarytool "$action" "$@" \
              --key "$notary_key" --key-id "$notary_key_id" --issuer "$notary_issuer"
    else
        xcrun notarytool "$action" "$@" \
              --apple-id "$notary_apple_id" --team-id "$notary_team_id" \
              --password "$notary_password"
    fi
}

# codesign gets its timestamp over plain HTTP from timestamp.apple.com, and on
# a network with an on-path HTTP proxy a percentage of those requests never
# reach Apple: the proxy answers 403 itself, securityd turns that into
# errSecTimestampServiceNotAvailable (-67885), and codesign prints "The
# timestamp service is not available." Apple is fine; the request was
# intercepted. Measured at roughly one request in twenty, arriving at random, so
# a retry is all it takes - but see ../vdjplugin/doc/signing.md, because a proxy
# rewriting Apple traffic is worth fixing rather than working around.
#
# The delay is short and flat on purpose. There is no server-side rate limit to
# back off from, so attempts are cheap and independent, and the only thing worth
# surviving is a bad window of a few seconds. Only timestamp failures are
# retried; a wrong identity or an unreadable bundle fails at once, where the
# message is useful. The tsa_ prefix on the variables is not decoration: sh
# functions have no locals, and an unprefixed "out" in here silently empties the
# global $out that the .pkg path is built from.
tsa_retry() {
    tsa_attempt=1
    while :; do
        if tsa_out=$("$@" 2>&1); then
            if [ -n "$tsa_out" ]; then printf '%s\n' "$tsa_out" | sed 's/^/  /'; fi
            return 0
        else
            tsa_status=$?
        fi
        case "$tsa_out" in
            *"timestamp service is not available"*|*"timestamp server"*|\
            *"Could not create a timestamp"*|*"Network error"*)
                ;;
            *)  printf '%s\n' "$tsa_out" | sed 's/^/  /' >&2
                return "$tsa_status" ;;
        esac
        if [ "$tsa_attempt" -ge 10 ]; then
            printf '%s\n' "$tsa_out" | sed 's/^/  /' >&2
            echo "  the timestamp request was refused $tsa_attempt times running." >&2
            echo "  A signature without a timestamp is no good for notarisation," >&2
            echo "  so this is fatal. Check whether something on the network is" >&2
            echo "  answering for timestamp.apple.com:" >&2
            echo "    curl -sS -D - -o /dev/null -X POST \\" >&2
            echo "         -H 'Content-Type: application/timestamp-query' \\" >&2
            echo "         http://timestamp.apple.com/ts01 | grep -i server" >&2
            return "$tsa_status"
        fi
        echo "  timestamp request intercepted or refused; retrying ($tsa_attempt/10)" >&2
        sleep 3
        tsa_attempt=$((tsa_attempt + 1))
    done
}

# The version the plug-ins were compiled with, so the receipt and the file name
# say the same thing the bundles do. k<Name>Version is a packed hex constant -
# 0x00010000 is 1.0.0 - and all three have to agree, because this is one package
# and a version that described only one of them would be a lie about the other
# two.
#
# Each *Version.h defines the constant twice, once per side of an #ifdef DEBUG,
# and the debug one is 0xFFFFFFFF - the Airwindows convention for "this is not a
# release". Taking the first match gets that one and calls the package 65535.255.255,
# so it is skipped by value rather than by counting lines.
packed_version=""
if [ -z "$version" ]; then
    for name in $plugin_names; do
        raw=$(sed -n "s/^[[:space:]]*#define[[:space:]]*k${name}Version[[:space:]]*0[xX]\([0-9A-Fa-f]*\).*/\1/p" \
              "$root/$name/${name}Version.h" |
              awk 'toupper($0) != "FFFFFFFF" { print; exit }')
        [ -n "$raw" ] || {
            echo "no release k${name}Version in $name/${name}Version.h; pass --version" >&2
            exit 1
        }
        decoded=$(printf '%d.%d.%d\n' \
                  $((0x$raw >> 16)) $(((0x$raw >> 8) & 255)) $((0x$raw & 255)))
        if [ -z "$version" ]; then
            version="$decoded"
            packed_version=$((0x$raw))
        elif [ "$version" != "$decoded" ]; then
            echo "the three plug-ins disagree about the version: $version vs $decoded ($name)." >&2
            echo "Line them up in the *Version.h files, or pass --version." >&2
            exit 1
        fi
    done
fi
[ -n "$version" ] || { echo "could not work out a version; pass --version" >&2; exit 1; }

if [ "$skip_build" -eq 0 ]; then
    if [ "$skip_tests" -eq 1 ]; then
        OUTDIR="$src" "$root/scripts/build.sh" --skip-tests
    else
        OUTDIR="$src" "$root/scripts/build.sh"
    fi
fi

echo "=== collect ==="
for name in $plugin_names; do
    comp="$src/$name.component"
    [ -x "$comp/Contents/MacOS/$name" ] || {
        echo "missing $comp/Contents/MacOS/$name" >&2
        echo "run scripts/build.sh first, or drop --skip-build." >&2
        exit 1
    }
    # The AudioComponents entry is what a host registers from. A component
    # without one loads and is never found, which is the hardest kind of broken
    # to notice, so it is checked here rather than discovered in a DAW.
    /usr/libexec/PlistBuddy -c "Print :AudioComponents:0:subtype" \
        "$comp/Contents/Info.plist" >/dev/null 2>&1 || {
        echo "$name.component has no AudioComponents entry in its Info.plist" >&2
        exit 1
    }
    # The version in there is the number a host displays, and it is written by
    # hand in the Info.plist while k<Name>Version is written by hand in the
    # header. Nothing makes them agree, so this does - skipped when --version
    # overrode the derivation and there is nothing to be consistent with.
    if [ -n "$packed_version" ]; then
        plist_version=$(/usr/libexec/PlistBuddy -c "Print :AudioComponents:0:version" \
                        "$comp/Contents/Info.plist" 2>/dev/null || echo "")
        [ "$plist_version" = "$packed_version" ] || {
            echo "$name: Info.plist says version $plist_version, k${name}Version says" >&2
            echo "$packed_version ($version). One of the two is out of date." >&2
            exit 1
        }
    fi
    archs=$(lipo -archs "$comp/Contents/MacOS/$name" 2>/dev/null || echo '?')
    printf '  %-20s %s\n' "$name.component" "$archs"
    case "$archs" in
        *x86_64*arm64*|*arm64*x86_64*) ;;
        *) echo "  note: not universal - a host running under the other" >&2
           echo "        architecture will not load this." >&2 ;;
    esac
done

# One clean tree per run: a stale component left in the payload from a previous
# build would be packaged without anything noticing.
rm -rf "$stage"
mkdir -p "$stage/root" "$stage/scripts" "$stage/resources" "$stage/pkgs"

for name in $plugin_names; do
    ditto "$src/$name.component" "$stage/root/$name.component"
done

if [ -n "$codesign_id" ]; then
    echo "=== codesign ==="
    for name in $plugin_names; do
        # --timestamp and --options runtime are what notarisation requires, and
        # neither works with an ad-hoc identity - so ad-hoc falls back to a
        # plain signature, which is all "-" can be. Anything going to Apple
        # takes the strict path or fails here, where the reason is legible.
        if [ "$codesign_id" = "-" ]; then
            codesign --force --sign - "$stage/root/$name.component"
        else
            tsa_retry codesign --force --timestamp --options runtime \
                      --sign "$codesign_id" "$stage/root/$name.component"
        fi
        codesign --verify --deep --strict "$stage/root/$name.component"
        printf '  %-20s %s\n' "$name.component" "$codesign_id"
    done
fi

# --- the postinstall --------------------------------------------------------
#
# The payload already landed in the right place; all this does is make the
# machine notice. AudioComponentRegistrar caches its scan, so a host opened
# straight after the install can otherwise still be working from the old list.
# It is launched on demand, so there is nothing to start again afterwards.
#
# $2 is the install destination, which for a home directory install is the home
# itself - the only thing here that knows whose machine this is, since the
# script may run as root and $HOME is then not to be trusted.

cat > "$stage/scripts/postinstall" <<'POSTINSTALL'
#!/bin/sh
set -eu

home="${2:-}"
case "$home" in
    ""|"/")
        user=$(stat -f '%Su' /dev/console 2>/dev/null || echo "")
        [ -n "$user" ] && home=$(dscl . -read "/Users/$user" NFSHomeDirectory 2>/dev/null |
                                 awk '{print $2}')
        [ -n "${home:-}" ] || home="${HOME:-}"
        ;;
esac

components="${home:-}/Library/Audio/Plug-Ins/Components"
if [ -n "${home:-}" ] && [ -d "$components" ]; then
    owner=$(stat -f '%Su:%Sg' "$home" 2>/dev/null || echo "")
    for name in Declick Dehum ParaEQ; do
        [ -d "$components/$name.component" ] && echo "installed $components/$name.component"
    done
    [ -n "$owner" ] && chown -R "$owner" "$components" 2>/dev/null || true
fi

killall -9 AudioComponentRegistrar >/dev/null 2>&1 || true
exit 0
POSTINSTALL
chmod 755 "$stage/scripts/postinstall"

# --- installer text ---------------------------------------------------------

cat > "$stage/resources/welcome.txt" <<WELCOME
ShellacFilters Audio Units $version

Three restoration plug-ins for any Audio Unit host - Logic, Live, Reaper,
Ableton, Audio Hijack, anything that scans for AUs:

Declick    detect-and-interpolate declicker for records and worn digital
Dehum      listens to the opening of the record to find the hum, then removes it
ParaEQ     console channel equaliser with the bands a disc transfer needs

All three are universal, so they load on Apple Silicon and on Intel, and all
three run the same DSP as their foobar2000 counterparts.

This installs into your home folder, not the system, because that is where a
plug-in you installed for yourself belongs. You will not be asked for an
administrator password.
WELCOME

cat > "$stage/resources/conclusion.txt" <<'CONCLUSION'
Installed.

Restart your host - the plug-in folder is scanned at startup - and the three
appear among the effects as ShellacFilters: Declick, Dehum and ParaEQ.

They are in

  ~/Library/Audio/Plug-Ins/Components

To remove them again, drag the three .component bundles out of that folder.
CONCLUSION

[ -f "$repo/LICENSE" ] && cp "$repo/LICENSE" "$stage/resources/LICENSE.txt"

# --- build the package ------------------------------------------------------

echo "=== pkgbuild ==="
pkgbuild --root "$stage/root" \
         --scripts "$stage/scripts" \
         --identifier "$identifier" \
         --version "$version" \
         --install-location "Library/Audio/Plug-Ins/Components" \
         "$stage/pkgs/component.pkg" >/dev/null

# customize="never" because there is one thing in here and nothing to choose
# between. The domains line is what makes this a home directory install - no
# admin password, and $2 in the postinstall is the home.
license_line=""
[ -f "$stage/resources/LICENSE.txt" ] &&
    license_line='<license file="LICENSE.txt" mime-type="text/plain"/>'

cat > "$stage/distribution.xml" <<DISTRIBUTION
<?xml version="1.0" encoding="utf-8"?>
<installer-gui-script minSpecVersion="2">
    <title>ShellacFilters Audio Units</title>
    <organization>com.shellacfilters</organization>
    <options customize="never" require-scripts="true" hostArchitectures="x86_64,arm64"/>
    <domains enable_anywhere="false" enable_currentUserHome="true" enable_localSystem="false"/>
    <welcome file="welcome.txt" mime-type="text/plain"/>
    <conclusion file="conclusion.txt" mime-type="text/plain"/>
    $license_line
    <choices-outline>
        <line choice="default">
            <line choice="$identifier"/>
        </line>
    </choices-outline>
    <choice id="default"/>
    <choice id="$identifier" visible="false">
        <pkg-ref id="$identifier"/>
    </choice>
    <pkg-ref id="$identifier" version="$version" onConclusion="none">component.pkg</pkg-ref>
</installer-gui-script>
DISTRIBUTION

pkg="$out/ShellacFilters-AudioUnits-$version.pkg"
mkdir -p "$out"
rm -f "$pkg"

echo "=== productbuild ==="
if [ -n "$sign" ]; then
    tsa_retry productbuild --distribution "$stage/distribution.xml" \
                           --package-path "$stage/pkgs" \
                           --resources "$stage/resources" \
                           --sign "$sign" \
                           "$pkg" >/dev/null
else
    productbuild --distribution "$stage/distribution.xml" \
                 --package-path "$stage/pkgs" \
                 --resources "$stage/resources" \
                 "$pkg" >/dev/null
fi

echo "=== verify ==="
# All three in the payload, or the package installs some of itself.
payload=$(pkgutil --payload-files "$pkg" 2>/dev/null || echo "")
for name in $plugin_names; do
    printf '%s\n' "$payload" | grep -q "$name.component/Contents/MacOS/$name" || {
        echo "$name is not in the package payload" >&2
        exit 1
    }
done
if [ -n "$sign" ]; then
    pkgutil --check-signature "$pkg" | sed 's/^/  /'
else
    echo "  unsigned - fine for this machine; Gatekeeper will complain about a"
    echo "  copy that has been downloaded. See --sign."
fi

# --- notarise ---------------------------------------------------------------
#
# Submit, wait, staple. Stapling is the half that is easy to leave out and
# whose absence only shows up on someone else's machine: without the ticket
# written into the package, Gatekeeper has to ask Apple at open time, and a
# laptop with no network then refuses a package that is in fact notarised.

if [ "$notarize" -eq 1 ]; then
    echo "=== notarize ==="
    echo "  submitting as $creds"
    log="$stage/notarytool.log"
    # || true because the interesting failures - Invalid, rejected credentials -
    # are in the output, and a bare non-zero exit here would lose it.
    notarytool_run submit "$pkg" --wait --timeout "$notary_timeout" 2>&1 |
        tee "$log" | sed 's/^/  /' || true

    # notarytool can exit 0 having reported a status of Invalid, so the status
    # line is what decides, not $?.
    if ! grep -q "status: Accepted" "$log"; then
        echo >&2
        echo "notarisation did not come back Accepted." >&2
        id=$(sed -n 's/^ *id: \([0-9a-f-][0-9a-f-]*\) *$/\1/p' "$log" | head -1)
        if [ -n "$id" ]; then
            echo "the notary's own account of why, for submission $id:" >&2
            notarytool_run log "$id" 2>&1 | sed 's/^/  /' >&2 || true
            echo >&2
            echo "again later with: xcrun notarytool log $id ..." >&2
        fi
        exit 1
    fi

    echo "=== staple ==="
    xcrun stapler staple "$pkg" | sed 's/^/  /'
    xcrun stapler validate "$pkg" | sed 's/^/  /'
    # What the machine it lands on will actually do with it. --type install is
    # the pkg assessment; the default type is for applications and passes
    # things a package installer would not.
    spctl --assess --verbose=2 --type install "$pkg" 2>&1 | sed 's/^/  /'
fi

printf '  %-42s %s\n' "$(basename "$pkg")" "$(du -h "$pkg" | awk '{print $1}')"
shasum -a 256 "$pkg" | sed 's/^/  /'

echo
echo "installer at $pkg"
echo
echo "Install it by double-clicking, or:"
echo "  installer -pkg \"$pkg\" -target CurrentUserHomeDirectory"
if [ "$notarize" -eq 0 ] && [ -n "$sign" ]; then
    echo
    echo "Signed but not notarised: add --notarize before handing it to anyone"
    echo "who will download it."
fi
