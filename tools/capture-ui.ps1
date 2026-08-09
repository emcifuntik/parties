param(
    [string]$BuildDirectory = "build",
    [string]$OutputDirectory = "build/ui-screenshots",
    [string]$Size = "1440x900",
    [string]$Profile = "",
    [int]$Frames = 240
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$designer = Join-Path $repository "$BuildDirectory/designer/rmlui_designer.exe"
$document = Join-Path $repository "client/ui/lobby.rml"
$contextDocument = Join-Path $repository "client/ui/context_window.rml"
$assets = Join-Path $repository "client/ui"
$output = Join-Path $repository $OutputDirectory

if (-not (Test-Path -LiteralPath $designer)) {
    throw "Designer executable not found: $designer"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null

$lobbyScenarios = @(
    "onboarding", "recovery", "launcher", "party-modal", "room",
    "stream-single", "stream-fps-overflow", "streams", "member", "settings",
    "settings-select-open", "settings-screen-share", "settings-hotkeys",
    "settings-account", "share", "audio-share", "chat", "chat-segment-churn"
)
if ($Profile -eq "iphone-17-pro") {
    # Sending a screen or application is unavailable on iOS, so those two
    # desktop-only picker fixtures intentionally do not belong in this profile.
    $lobbyScenarios = $lobbyScenarios | Where-Object {
        $_ -ne "share" -and $_ -ne "audio-share"
    }
}
foreach ($scenario in $lobbyScenarios) {
    $screenshot = Join-Path $output "$scenario.bmp"
    $designerArgs = @($document, "--asset-dir", $assets, "--fixture", $scenario)
    if ($Profile) {
        $designerArgs += @("--profile", $Profile)
    } else {
        $designerArgs += @("--size", $Size)
    }
    $designerArgs += @("--frames", "$Frames", "--screenshot", $screenshot)
    & $designer @designerArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Screenshot scenario failed: $scenario"
    }
}

$contextScenarios = if ($Profile) { @() } else { @(
        @{ Name = "context-user"; Size = "420x520" },
        @{ Name = "context-channel"; Size = "310x310" }
    ) }
foreach ($scenario in $contextScenarios) {
    $screenshot = Join-Path $output "$($scenario.Name).bmp"
    & $designer $contextDocument --asset-dir $assets --fixture $scenario.Name `
        --size $scenario.Size --frames $Frames --screenshot $screenshot
    if ($LASTEXITCODE -ne 0) {
        throw "Screenshot scenario failed: $($scenario.Name)"
    }
}

$count = $lobbyScenarios.Count + $contextScenarios.Count
Write-Host "Captured $count UI scenarios in $output"
