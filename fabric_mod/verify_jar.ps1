param([string]$JarPath)

Add-Type -AssemblyName System.IO.Compression.FileSystem
$fullPath = (Resolve-Path $JarPath).Path
$archive = [System.IO.Compression.ZipFile]::OpenRead($fullPath)
try {
    $names = @($archive.Entries | ForEach-Object { $_.FullName })
    $required = @('fabric.mod.json', 'com/overlay/CoordsClientMod.class', 'com/overlay/PlayerNew.class')
    $missing = @($required | Where-Object { $_ -notin $names })

    Write-Host "Files checked in $JarPath`:`n"
    $names | Where-Object { $_ -in $required }

    if ($missing.Count -gt 0) {
        Write-Host "[FAIL] Missing: $($missing -join ', ')" -ForegroundColor Red
        exit 1
    }

    Write-Host "[OK] JAR contains Fabric metadata and compiled mod class." -ForegroundColor Green
} finally {
    $archive.Dispose()
}
