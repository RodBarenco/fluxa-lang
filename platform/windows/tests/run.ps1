param(
    [Parameter(Mandatory = $true)]
    [string]$Fluxa
)

$ErrorActionPreference = 'Continue'
$tests = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Resolve-Path (Join-Path $tests '..\..\..')
$runtime = (Resolve-Path -LiteralPath $Fluxa).Path
$passed = 0
$failed = 0

function Invoke-FluxaCase {
    param(
        [string]$Name,
        [string]$WorkingDirectory,
        [string]$File,
        [string[]]$Expected
    )

    Push-Location -LiteralPath $WorkingDirectory
    try {
        $lines = @(
            & $runtime run $File 2>&1 |
                ForEach-Object { "$_".TrimEnd() } |
                Where-Object { $_ -notmatch '^(INFO|DEBUG|TRACE): ' }
        )
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
    }

    if ($code -eq 0 -and (@($lines) -join "`n") -eq (@($Expected) -join "`n")) {
        Write-Host "  PASS  $Name"
        $script:passed++
        return
    }

    Write-Host "  FAIL  $Name"
    Write-Host "    exit: $code"
    Write-Host "    expected: $(@($Expected) -join ' | ')"
    Write-Host "    actual:   $(@($lines) -join ' | ')"
    $script:failed++
}

Write-Host "Windows PE dependency gate"
$objdump = Get-Command objdump.exe -ErrorAction Stop
$imports = @(
    & $objdump.Source -p $runtime |
        Select-String 'DLL Name:' |
        ForEach-Object { ($_ -split 'DLL Name:')[1].Trim().ToLowerInvariant() }
)
$allowed = @(
    'advapi32.dll', 'bcrypt.dll', 'crypt32.dll', 'gdi32.dll',
    'iphlpapi.dll', 'kernel32.dll', 'msvcrt.dll', 'opengl32.dll',
    'secur32.dll', 'shell32.dll', 'user32.dll', 'winmm.dll',
    'wldap32.dll', 'ws2_32.dll'
)
$external = @($imports | Where-Object { $_ -notin $allowed })
if ($external.Count -eq 0) {
    Write-Host "  PASS  standalone/system-dlls-only"
    $passed++
} else {
    Write-Host "  FAIL  standalone/system-dlls-only: $($external -join ', ')"
    $failed++
}

Invoke-FluxaCase `
    -Name 'language/core' `
    -WorkingDirectory $tests `
    -File 'language.flx' `
    -Expected @('42', '10', '1', '2', 'danger-ok')

Invoke-FluxaCase `
    -Name 'stdlib/essential' `
    -WorkingDirectory $tests `
    -File 'stdlib.flx' `
    -Expected @(
        'FLUXA', '42', 'true', 'hello-windows', 'true', '7', '32',
        'raylib/6.0', 'std.image 1.0 (raylib codec)',
        'miniaudio/0.11.25', '2', '2'
    )

if ($env:FLUXA_WINDOWS_NETWORK_TESTS -eq '1') {
    Invoke-FluxaCase `
        -Name 'stdlib/network' `
        -WorkingDirectory $tests `
        -File 'network.flx' `
        -Expected @('200', '200', 'true')
} else {
    Write-Host '  SKIP  stdlib/network (set FLUXA_WINDOWS_NETWORK_TESTS=1)'
}

Write-Host "Windows results: $passed passed, $failed failed"
if ($failed -ne 0) {
    exit 1
}
