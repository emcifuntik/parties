param(
    [string]$BuildDirectory = "build",
    [string]$OutputDirectory = "build/ui-screenshots",
    [string]$Size = "1440x900",
    [string]$Profile = "",
    [int]$Frames = 240,
    # Also capture the secondary states that have no layout contract (dialogs,
    # banners, onboarding steps) and save every picture as PNG. This is the
    # full set a designer needs as a reference; it is not a regression run.
    [switch]$Reference
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
if ($Reference) {
    $lobbyScenarios = @($lobbyScenarios | Where-Object { $_ -ne "chat-segment-churn" }) + @(
        "room-camera", "streams-camera", "settings-camera", "diagnostics",
        "onboarding-restore", "onboarding-key-import", "global-name",
        "launcher-reconnecting", "login", "tofu", "update-available", "room-empty",
        "create-channel", "rename-channel", "create-text-channel", "server-nickname",
        "chat-draft", "chat-search", "chat-pinned"
    )
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
        # A reference run wants the picture even when a layout check fails.
        if ($Reference -and (Test-Path -LiteralPath $screenshot)) {
            Write-Warning "Layout checks failed for $scenario; the screenshot was kept"
        } else {
            throw "Screenshot scenario failed: $scenario"
        }
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

if ($Reference) {
    Add-Type -AssemblyName System.Drawing
    Get-ChildItem -LiteralPath $output -Filter *.bmp | ForEach-Object {
        $png = [System.IO.Path]::ChangeExtension($_.FullName, ".png")
        $image = [System.Drawing.Image]::FromFile($_.FullName)
        try { $image.Save($png, [System.Drawing.Imaging.ImageFormat]::Png) } finally { $image.Dispose() }
        Remove-Item -LiteralPath $_.FullName
    }
}

$count = $lobbyScenarios.Count + $contextScenarios.Count
Write-Host "Captured $count UI scenarios in $output"
