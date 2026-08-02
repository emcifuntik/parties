param(
    [string]$BuildDirectory = "build",
    [string]$OutputDirectory = "build/ui-screenshots",
    [string]$Size = "1440x900",
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
    "stream-single", "streams", "member", "settings", "share", "audio-share", "chat"
)
foreach ($scenario in $lobbyScenarios) {
    $screenshot = Join-Path $output "$scenario.bmp"
    & $designer $document --asset-dir $assets --fixture $scenario `
        --size $Size --frames $Frames --screenshot $screenshot
    if ($LASTEXITCODE -ne 0) {
        throw "Screenshot scenario failed: $scenario"
    }
}

$contextScenarios = @(
    @{ Name = "context-user"; Size = "420x520" },
    @{ Name = "context-channel"; Size = "310x310" }
)
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
