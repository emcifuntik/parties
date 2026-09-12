#!/bin/zsh
set -euo pipefail

repository="${0:A:h:h}"
app="${1:-$repository/build-ios-simulator/client/Debug-iphonesimulator/parties_client.app}"
output="${2:-$repository/build/ui-screenshots-ios}"
device="${3:-booted}"
bundle_id="com.parties.ios"

if [[ ! -x "$app/parties_client" ]]; then
    print -u2 "iOS Simulator client not found: $app"
    exit 2
fi

mkdir -p "$output"
output="${output:A}"
xcrun simctl install "$device" "$app"

scenarios=(
    launcher launcher-reconnecting sidebar island-idle island-long-name party-modal onboarding onboarding-restore
    onboarding-key-import recovery room room-empty chat chat-draft chat-search chat-pinned chat-attachment chat-keyboard
    settings settings-select-open settings-airpods settings-screen-share
    settings-hotkeys settings-account settings-account-import stream-single stream-fullscreen streams member login login-existing tofu
    create-channel create-text-channel rename-channel global-name server-nickname
)

if [[ -n "${UI_SCENARIOS:-}" ]]; then scenarios=(${=UI_SCENARIOS}); fi

fixture_pid=0
audit_failed=0
cleanup_fixture() {
    if (( fixture_pid > 0 )) && kill -0 "$fixture_pid" 2>/dev/null; then
        xcrun simctl terminate "$device" "$bundle_id" >/dev/null 2>&1 || true
    fi
    fixture_pid=0
}
trap cleanup_fixture EXIT INT TERM

for scenario in $scenarios; do
    print "Capturing $scenario"
    log_file="$output/$scenario.log"
    : > "$log_file"
    launch_result="$(xcrun simctl launch --terminate-running-process \
        --stdout="$log_file" --stderr="$log_file" \
        "$device" "$bundle_id" --ui-fixture "$scenario")"
    fixture_pid="${launch_result##*: }"

    ready=0
    for attempt in {1..300}; do
        if ! kill -0 "$fixture_pid" 2>/dev/null; then
            print -u2 "Fixture exited before capture: $scenario"
            cat "$log_file" >&2
            exit 1
        fi
        if /usr/bin/grep -Fq "Ready iOS UI fixture: $scenario" "$log_file"; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if (( ! ready )); then
        print -u2 "Timed out waiting for fixture: $scenario"
        cat "$log_file" >&2
        exit 1
    fi

    # Let UIKit transitions and nested RmlUI data bindings settle.
    sleep 0.8
    kill -0 "$fixture_pid"
    xcrun simctl io "$device" screenshot "$output/$scenario.png"
    if /usr/bin/grep -Ei 'RmlUi.*(error|assert)|\[error\]|\[assert\]|Failed to (initialize|load|render)|Unknown iOS UI fixture' "$log_file"; then
        print -u2 "Fixture reported a runtime error: $scenario"
        exit 1
    fi
    if /usr/bin/grep -Fq '[UI audit] FAIL' "$log_file"; then
        print -u2 "Control layout audit failed: $scenario"
        audit_failed=1
    fi
    cleanup_fixture
done

print "Captured ${#scenarios} iOS UI scenarios in $output"

exit "$audit_failed"
