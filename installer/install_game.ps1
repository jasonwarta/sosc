# Copies Streets of SimCity from its CD image into the install folder, laid out the way the fix
# expects: the CD's exe folder at the top plus the game's data folders. Run by Setup.exe.
#   -Source  the CD image (.iso) or the oldgamesdownload.com zip that holds it
#   -Dest    the install folder
# The image is read directly (ISO 9660, with Joliet long names when present), so nothing is
# mounted and no other tools are needed. Writes logs\install_game.log in Dest. Exit 0 on success.
param([Parameter(Mandatory)][string]$Source, [Parameter(Mandatory)][string]$Dest)

$ErrorActionPreference = 'Stop'
$folders = 'BMP', 'Cities', 'FONTS', 'Games', 'GEO', 'HELP', 'scenarios', 'shared', 'SMK', 'SOUND', 'Weapons', 'X'
New-Item -ItemType Directory -Force (Join-Path $Dest 'logs') | Out-Null
$log = Join-Path $Dest 'logs\install_game.log'
function Log($msg) { "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $msg" | Add-Content -Path $log -Encoding utf8 }

$Sector = 2048
$script:iso = $null
$script:joliet = $false

function Read-Bytes([long]$offset, [int]$count) {
    $buf = New-Object byte[] $count
    $script:iso.Position = $offset
    $got = 0
    while ($got -lt $count) {
        $n = $script:iso.Read($buf, $got, $count - $got)
        if ($n -le 0) { throw "the image ends early (offset $offset)" }
        $got += $n
    }
    return , $buf
}

# Directory records of the directory at sector $lba, $size bytes long: name, lba, size, isDir.
function Get-Entries([long]$lba, [long]$size) {
    $data = Read-Bytes ($lba * $Sector) $size
    $entries = @()
    $i = 0
    while ($i -lt $size) {
        $len = $data[$i]
        if ($len -eq 0) { $i = ([math]::Floor($i / $Sector) + 1) * $Sector; continue }  # records don't cross sectors
        $nameLen = $data[$i + 32]
        $raw = [byte[]]$data[($i + 33)..($i + 32 + $nameLen)]
        if (-not ($nameLen -eq 1 -and $raw[0] -le 1)) {  # skip . and ..
            $name = if ($script:joliet) { [Text.Encoding]::BigEndianUnicode.GetString($raw) } else { [Text.Encoding]::ASCII.GetString($raw) }
            $name = $name -replace ';\d+$', ''
            if (-not $script:joliet) { $name = $name -replace '\.$', '' }
            $entries += [pscustomobject]@{
                Name  = $name
                Lba   = [BitConverter]::ToUInt32($data, $i + 2)
                Size  = [BitConverter]::ToUInt32($data, $i + 10)
                IsDir = ($data[$i + 25] -band 2) -ne 0
            }
        }
        $i += $len
    }
    return $entries
}

function Copy-Tree($dir, [string]$to) {
    New-Item -ItemType Directory -Force $to | Out-Null
    foreach ($e in Get-Entries $dir.Lba $dir.Size) {
        $path = Join-Path $to $e.Name
        if ($e.IsDir) { Copy-Tree $e $path; continue }
        $out = [IO.File]::Create($path)
        try {
            $script:iso.Position = [long]$e.Lba * $Sector
            $left = [long]$e.Size
            $buf = New-Object byte[] 1048576
            while ($left -gt 0) {
                $n = $script:iso.Read($buf, 0, [int][math]::Min($buf.Length, $left))
                if ($n -le 0) { throw "the image ends early in $($e.Name)" }
                $out.Write($buf, 0, $n)
                $left -= $n
            }
        } finally { $out.Dispose() }
        $script:files++
        $script:bytes += $e.Size
    }
}

$tempIso = $null
try {
    Log "source: $Source"
    Log "destination: $Dest"
    $isoPath = $Source
    if ($Source -match '\.zip$') {
        # The oldgamesdownload.com zip: take the .iso out of it.
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($Source)
        try {
            $entry = $zip.Entries | Where-Object { $_.Name -match '\.iso$' } | Select-Object -First 1
            if (-not $entry) { throw "no .iso inside $Source" }
            $tempIso = Join-Path $env:TEMP "streets_$PID.iso"
            Log "extracting $($entry.FullName) ($($entry.Length) bytes)"
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $tempIso, $true)
            $isoPath = $tempIso
        } finally { $zip.Dispose() }
    }
    $script:iso = [IO.File]::OpenRead((Resolve-Path $isoPath).Path)

    # Volume descriptors from sector 16: 1 = primary, 2 = supplementary (Joliet when its escape
    # sequence is %/@, %/C or %/E), 255 = end. Prefer Joliet for the long, mixed-case names.
    $root = $null
    for ($s = 16; $s -lt 64; $s++) {
        $vd = Read-Bytes ($s * $Sector) $Sector
        if ([Text.Encoding]::ASCII.GetString($vd, 1, 5) -ne 'CD001') { throw "$isoPath isn't a CD image" }
        if ($vd[0] -eq 255) { break }
        $isJoliet = $vd[0] -eq 2 -and $vd[88] -eq 0x25 -and $vd[89] -eq 0x2F -and $vd[90] -in 0x40, 0x43, 0x45
        if ($vd[0] -eq 1 -and -not $root -or $isJoliet) {
            $root = [pscustomobject]@{ Lba = [BitConverter]::ToUInt32($vd, 156 + 2); Size = [BitConverter]::ToUInt32($vd, 156 + 10) }
            $script:joliet = $isJoliet
        }
    }
    if (-not $root) { throw "no ISO 9660 volume in $isoPath" }
    Log "reading $isoPath ($(if ($script:joliet) { 'Joliet' } else { 'ISO 9660' }) names)"

    $top = Get-Entries $root.Lba $root.Size
    $exe = $top | Where-Object { $_.IsDir -and $_.Name -eq 'exe' }
    if (-not $exe -or -not ($top | Where-Object { $_.IsDir -and $_.Name -eq 'GEO' })) {
        throw "$isoPath isn't the Streets of SimCity CD (no exe and GEO folders)"
    }
    $script:files = 0
    $script:bytes = 0
    Copy-Tree $exe $Dest
    Log "copied exe"
    foreach ($f in $folders) {
        $d = $top | Where-Object { $_.IsDir -and $_.Name -eq $f }
        if (-not $d) { Log "skipped $f (not on this CD)"; continue }
        Copy-Tree $d (Join-Path $Dest $d.Name)
        Log "copied $($d.Name)"
    }
    # Streets.exe links to glide2x.dll, so Windows won't start it without one beside it. The CD
    # keeps the original 3dfx one in shared\. Without 3dfx hardware it only fails a hardware
    # probe at startup (a few caught exceptions); the OpenGL renderer never uses it.
    $glide = Join-Path $Dest 'glide2x.dll'
    if (-not (Test-Path $glide) -and (Test-Path (Join-Path $Dest 'shared\glide2x.dll'))) {
        Copy-Item (Join-Path $Dest 'shared\glide2x.dll') $glide
        Log "copied shared\glide2x.dll beside Streets.exe"
    }
    Log "done: $($script:files) files, $([math]::Round($script:bytes / 1MB)) MB"
    exit 0
} catch {
    Log "FAILED: $_"
    exit 1
} finally {
    if ($script:iso) { $script:iso.Dispose() }
    if ($tempIso -and (Test-Path $tempIso)) { Remove-Item $tempIso -Force }
}
