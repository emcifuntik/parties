#!/bin/zsh
set -euo pipefail

repository="${0:A:h:h}"
app="${1:-$repository/build-macos/client/parties_client.app}"
output="${2:-$repository/build/ui-screenshots-macos}"
executable="$app/Contents/MacOS/parties_client"
window_helper="$repository/build/macos-window-id"
log_file="$output/capture.log"

if [[ ! -x "$executable" ]]; then
    print -u2 "macOS client not found: $executable"
    exit 2
fi

mkdir -p "$output"
/usr/bin/xcrun swiftc "$repository/tools/macos-window-id.swift" -o "$window_helper"
: > "$log_file"

scenarios=(
    launcher launcher-reconnecting update-available party-modal
    onboarding onboarding-restore onboarding-key-import recovery
    login login-existing tofu global-name server-nickname
    room room-empty chat chat-search chat-pinned chat-attachment
    settings settings-select-open settings-screen-share settings-hotkeys
    settings-account settings-account-import
    stream-single streams create-channel create-text-channel rename-channel
    member native-user native-channel native-server native-message
    native-share-picker native-audio-picker
)

fixture_pid=0
cleanup_fixture() {
    if (( fixture_pid > 0 )) && kill -0 "$fixture_pid" 2>/dev/null; then
        kill "$fixture_pid" 2>/dev/null || true
        wait "$fixture_pid" 2>/dev/null || true
    fi
    fixture_pid=0
}
trap cleanup_fixture EXIT INT TERM

for scenario in $scenarios; do
    print "Capturing $scenario"
    "$executable" --ui-fixture "$scenario" >> "$log_file" 2>&1 &
    fixture_pid=$!

    window_id=""
    for attempt in {1..100}; do
        if ! kill -0 "$fixture_pid" 2>/dev/null; then
            print -u2 "Fixture exited before opening a window: $scenario"
            tail -80 "$log_file" >&2
            exit 1
        fi
        if window_id="$($window_helper "$fixture_pid" 2>/dev/null)" && [[ -n "$window_id" ]]; then
            break
        fi
        sleep 0.05
    done
    if [[ -z "$window_id" ]]; then
        print -u2 "Timed out waiting for fixture window: $scenario"
        exit 1
    fi

    # Data bindings settle in the first frame; the delay also gives AppKit
    # enough time to animate native menus and popovers into their final shape.
    if [[ "$scenario" == native-* ]]; then
        sleep 1.25
    else
        sleep 0.45
    fi
    if [[ "$scenario" == "native-share-picker" || "$scenario" == "native-audio-picker" ]]; then
        # SCContentSharingPicker is a system-wide overlay, not an application
        # child window, so a window-only capture intentionally cannot see it.
        /usr/sbin/screencapture -x "$output/$scenario.png"
    else
        /usr/sbin/screencapture -x -l "$window_id" "$output/$scenario.png"
    fi
    cleanup_fixture
done

print "Captured ${#scenarios} macOS UI scenarios in $output"
