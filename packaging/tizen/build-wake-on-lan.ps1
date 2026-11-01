[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$EmsdkRoot
)

# Builds the Wake-on-LAN WebAssembly module into $OutputDirectory. Tizen Sockets exist only in
# Samsung's Emscripten fork (1.39.4.7, fastcomp), available for Linux, macOS and Windows from
# https://developer.samsung.com/smarttv/develop/extension-libraries/webassembly/download.html

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($EmsdkRoot)) {
    $EmsdkRoot = if ($env:SAMSUNG_EMSDK) { $env:SAMSUNG_EMSDK } else {
        Join-Path $env:USERPROFILE 'samsung-emscripten\emscripten-release-bundle\emsdk'
    }
}
$EmsdkRoot = [System.IO.Path]::GetFullPath($EmsdkRoot)
$emscripten = Join-Path $EmsdkRoot 'fastcomp\emscripten'
if (-not (Test-Path -LiteralPath (Join-Path $emscripten 'src\library_tizen_sockfs.js') -PathType Leaf)) {
    throw "Samsung's Emscripten SDK was not found at $EmsdkRoot. Pass -EmsdkRoot or set SAMSUNG_EMSDK to its emsdk directory."
}

$config = Join-Path $EmsdkRoot '.emscripten'
if (-not (Test-Path -LiteralPath $config -PathType Leaf)) {
    # Keeps the configuration inside the SDK instead of the user's home directory.
    & (Join-Path $EmsdkRoot 'emsdk.bat') activate latest-fastcomp --embedded | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $config -PathType Leaf)) {
        throw "Unable to activate the Emscripten SDK at $EmsdkRoot."
    }
}

$python = Get-ChildItem -LiteralPath (Join-Path $EmsdkRoot 'python') -Filter python.exe -Recurse -File |
    Select-Object -First 1
$node = Get-ChildItem -LiteralPath (Join-Path $EmsdkRoot 'node') -Filter node.exe -Recurse -File |
    Select-Object -First 1
if (-not $python -or -not $node) {
    throw "The Emscripten SDK at $EmsdkRoot does not contain its bundled Python and Node."
}

$source = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\tizen\wasm\wake-on-lan.c'))
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$output = Join-Path ([System.IO.Path]::GetFullPath($OutputDirectory)) 'wake-on-lan.js'

$savedEnvironment = @{ PATH = $env:PATH; EMSDK = $env:EMSDK; EM_CONFIG = $env:EM_CONFIG; EM_CACHE = $env:EM_CACHE }
try {
    $env:EMSDK = $EmsdkRoot
    $env:EM_CONFIG = $config
    $env:EM_CACHE = Join-Path $EmsdkRoot '.emscripten_cache'
    $env:PATH = "$emscripten;$($python.DirectoryName);$($node.DirectoryName);$env:PATH"

    # Sockets are refused on the browser main thread, so the module sends from a pthread; the
    # pool keeps one worker ready so the first wake does not wait for a thread to start.
    $arguments = @(
        $source, '-Os', '-Wall', '-Wextra', '-Werror',
        '-s', 'WASM=1',
        '-s', 'ENVIRONMENT_MAY_BE_TIZEN=1',
        '-s', 'USE_PTHREADS=1',
        '-s', 'PTHREAD_POOL_SIZE=1',
        '-s', 'TOTAL_MEMORY=16777216',
        '-s', 'MODULARIZE=1',
        '-s', 'EXPORT_NAME=createWakeOnLanModule',
        '-s', "EXPORTED_FUNCTIONS=['_wol_send']",
        '-s', "EXTRA_EXPORTED_RUNTIME_METHODS=['ccall']",
        '-o', $output
    )
    & (Join-Path $emscripten 'emcc.bat') @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "The Wake-on-LAN module failed to build with exit code $LASTEXITCODE."
    }
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        if ($null -eq $savedEnvironment[$name]) {
            Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue
        } else {
            Set-Item -Path "Env:$name" -Value $savedEnvironment[$name]
        }
    }
}

foreach ($artifact in @('wake-on-lan.js', 'wake-on-lan.wasm', 'wake-on-lan.worker.js', 'wake-on-lan.js.mem')) {
    if (-not (Test-Path -LiteralPath (Join-Path $OutputDirectory $artifact) -PathType Leaf)) {
        throw "The Wake-on-LAN build did not produce $artifact."
    }
}
Write-Host "Wake-on-LAN module built: $OutputDirectory"
