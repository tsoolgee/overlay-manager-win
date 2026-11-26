# Builds Floaty-offline.exe: Floaty.exe with Microsoft's WebView2 Fixed Version
# runtime appended, for a Windows 10 machine with no internet and no runtime.
# The program reads the .cab back from its own tail (see src/runtime.cpp).
#
#   tools/make-offline.ps1 -Exe build/Release/Floaty.exe -Cab <...FixedVersionRuntime...x64.cab> [-Out dist/Floaty-offline.exe]
#
# The .cab comes from https://developer.microsoft.com/microsoft-edge/webview2/
# (Fixed Version, x64). ASCII only: Windows PowerShell 5.1 reads it as ANSI.
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(Mandatory = $true)][string]$Cab,
    [string]$Out = (Join-Path (Split-Path $Exe) 'Floaty-offline.exe')
)
$ErrorActionPreference = 'Stop'

$sig = Get-AuthenticodeSignature $Cab
if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
    throw "$Cab is not a valid Microsoft-signed package ($($sig.Status))"
}

$cabSize = (Get-Item $Cab).Length
$out = [IO.File]::Create($Out)
try {
    foreach ($part in @($Exe, $Cab)) {
        $in = [IO.File]::OpenRead($part)
        try { $in.CopyTo($out, 4MB) } finally { $in.Dispose() }
    }
    # Trailer: 16-byte magic (15 chars + NUL), then the .cab size as a little-endian uint64.
    $magic = [Text.Encoding]::ASCII.GetBytes('FloatyRuntime01') + [byte]0
    $out.Write($magic, 0, 16)
    $size = [BitConverter]::GetBytes([uint64]$cabSize)
    $out.Write($size, 0, 8)
} finally {
    $out.Dispose()
}

$hash = (Get-FileHash $Out -Algorithm SHA256).Hash
"{0}  {1:N0} bytes  SHA-256 {2}" -f $Out, (Get-Item $Out).Length, $hash
