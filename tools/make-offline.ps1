# Builds Floaty-offline.exe: Floaty.exe with Microsoft's WebView2 Fixed Version
# runtime appended, for a Windows 10 machine with no internet and no runtime.
# The program reads the .cab back from its own tail (see src/runtime.cpp).
#
#   tools/make-offline.ps1 -Exe build/Release/Floaty.exe -Cab <Microsoft.WebView2.FixedVersionRuntime.*.x64.cab> [-Out <path>]
#
# -Cab is Microsoft's package as downloaded from
# https://developer.microsoft.com/microsoft-edge/webview2/ (Fixed Version, x64);
# its signature is checked. The runtime is then slimmed and packed again:
#   - Locales: only he and en-US (the rest is ~135 MB of other languages);
#   - WidevineCdm: the DRM module for protected video, never used here;
#   - EBWebView\x86: the 32-bit host side; Floaty is 64-bit;
# and recompressed with LZX:21 (Microsoft ships it with the weaker MSZIP).
# expand.exe, which unpacks it on the target, reads LZX.
# ASCII only: Windows PowerShell 5.1 reads it as ANSI.
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

# Work next to the output, not in %TEMP%: about 1 GB passes through, and
# makecab reads its directive file as ANSI, so a path with Hebrew in it (a
# user profile, say) would not survive. The output folder must be plain ASCII.
$outDir = Split-Path ([IO.Path]::GetFullPath($Out))
if ($outDir -match '[^ -~]') { throw "Put -Out in a folder whose path is plain ASCII: $outDir" }
$work = Join-Path $outDir ("floaty-offline-work-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
try {
    $tree = Join-Path $work 'tree'
    New-Item -ItemType Directory $tree | Out-Null
    & "$env:SystemRoot\System32\expand.exe" $Cab -F:* $tree | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "expand failed ($LASTEXITCODE)" }
    $runtime = Get-ChildItem $tree -Directory | Select-Object -First 1
    if (-not $runtime -or -not (Test-Path (Join-Path $runtime.FullName 'msedgewebview2.exe'))) {
        throw 'no runtime folder in the package'
    }
    $r = $runtime.FullName
    Get-ChildItem (Join-Path $r 'Locales') -File | Where-Object { $_.Name -notin @('he.pak', 'en-US.pak') } | Remove-Item
    foreach ($dir in 'WidevineCdm', 'EBWebView\x86') {
        $p = Join-Path $r $dir
        if (Test-Path $p) { Remove-Item -Recurse -Force $p }
    }

    # makecab directive: one folder, LZX:21, the tree under its own top folder.
    $packed = Join-Path $work 'runtime.cab'
    $lines = @(
        '.Set CabinetNameTemplate=runtime.cab', ".Set DiskDirectoryTemplate=$work",
        '.Set Cabinet=on', '.Set Compress=on', '.Set CompressionType=LZX', '.Set CompressionMemory=21',
        '.Set MaxDiskSize=0', '.Set CabinetFileCountThreshold=0', '.Set FolderSizeThreshold=0',
        '.Set MaxCabinetSize=0', '.Set UniqueFiles=off', '.Set RptFileName=nul', '.Set InfFileName=nul'
    )
    $last = $null
    foreach ($f in (Get-ChildItem $tree -Recurse -File | Sort-Object FullName)) {
        $rel = $f.DirectoryName.Substring($tree.Length).TrimStart('\')
        if ($rel -ne $last) { $lines += ".Set DestinationDir=`"$rel`""; $last = $rel }
        $lines += "`"$($f.FullName)`" `"$($f.Name)`""
    }
    $ddf = Join-Path $work 'runtime.ddf'
    [IO.File]::WriteAllLines($ddf, $lines, [Text.Encoding]::ASCII)
    & "$env:SystemRoot\System32\makecab.exe" /F $ddf | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $packed)) { throw "makecab failed ($LASTEXITCODE)" }

    $cabSize = (Get-Item $packed).Length
    $out = [IO.File]::Create($Out)
    try {
        foreach ($part in @($Exe, $packed)) {
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
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

$hash = (Get-FileHash $Out -Algorithm SHA256).Hash
"{0}  {1:N0} bytes  SHA-256 {2}" -f $Out, (Get-Item $Out).Length, $hash
