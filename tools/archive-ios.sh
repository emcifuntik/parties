#!/bin/zsh
set -euo pipefail

repository="${0:A:h:h}"
archive="${1:-$repository/build/ios/Parties.xcarchive}"
team="${2:-${APPLE_DEVELOPMENT_TEAM:-}}"
cmake_command="${CMAKE:-cmake}"

if [[ -z "$team" ]]; then
    print -u2 "Provide an Apple team ID as the second argument or APPLE_DEVELOPMENT_TEAM."
    exit 2
fi

archive="${archive:A}"
cd "$repository"
"$cmake_command" --preset ios-arm64-release \
    -DBUILD_IN_RETAIL=ON -DAPPLE_DEVELOPMENT_TEAM="$team"
xcodebuild -project build-ios-release/parties.xcodeproj \
    -scheme parties_client -configuration Release \
    -destination 'generic/platform=iOS' -archivePath "$archive" \
    -allowProvisioningUpdates archive

# CMake places the dSYM in its custom configuration build directory. Xcode
# does not automatically collect it into the archive from that location.
symbols="$repository/build-ios-release/client/Release-iphoneos/parties_client.app.dSYM"
binary="$archive/Products/Applications/parties_client.app/parties_client"
if [[ ! -d "$symbols" ]]; then
    print -u2 "Missing application debug symbols: $symbols"
    exit 1
fi
binary_uuid="$(xcrun dwarfdump --uuid "$binary" | /usr/bin/awk '{print $2, $3}')"
symbols_uuid="$(xcrun dwarfdump --uuid "$symbols" | /usr/bin/awk '{print $2, $3}')"
if [[ -z "$binary_uuid" || "$binary_uuid" != "$symbols_uuid" ]]; then
    print -u2 "Application and dSYM UUIDs do not match."
    exit 1
fi
mkdir -p "$archive/dSYMs"
ditto "$symbols" "$archive/dSYMs/parties_client.app.dSYM"
codesign --verify --deep --strict "${binary:h}"
print "Verified signed iOS archive with matching debug symbols: $archive"
