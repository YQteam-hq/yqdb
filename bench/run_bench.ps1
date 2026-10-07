param(
    [int]$N = 100000,
    [string]$Modes = "normal,nosync",
    [string]$Orders = "rand,seq"
)

$ErrorActionPreference = "Stop"
$env:Path = "C:\mingw64\mingw64\bin;" + $env:Path

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$proj = Split-Path -Parent $root
$tp   = Join-Path $root "third_party"
$bd   = Join-Path $root "build"
$dat  = Join-Path $root "data"

New-Item -ItemType Directory -Force -Path $bd  | Out-Null
New-Item -ItemType Directory -Force -Path $dat | Out-Null

$core = @("yq","yq_btree","yq_enc","yq_memblk","yq_memtable","yq_mvcc","yq_recover","yq_slice","yq_vfs","yq_wal")
$objs = @()
foreach ($f in $core) {
    $o = Join-Path $bd "$f.o"
    $objs += $o
    if (-not (Test-Path $o) -or (Get-Item (Join-Path $proj "src\$f.c")).LastWriteTime -gt (Get-Item $o).LastWriteTime) {
        gcc -O2 -std=c11 -I (Join-Path $proj "include") -c (Join-Path $proj "src\$f.c") -o $o
        if ($LASTEXITCODE -ne 0) { throw "core compile failed: $f" }
    }
}

Write-Output "=== building benchmarks ==="

function Get-CachedObj {
    param([string]$Src, [string[]]$Extra, [string]$OutName)
    $o = Join-Path $bd $OutName
    if (-not (Test-Path $o) -or (Get-Item $Src).LastWriteTime -gt (Get-Item $o).LastWriteTime) {
        $gargs = @("-O2","-std=c11","-I",$tp) + $Extra + @("-c",$Src,"-o",$o)
        & gcc @gargs
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $Src" }
    }
    return $o
}

$sqliteObj = Get-CachedObj -Src (Join-Path $tp "sqlite3.c") -Extra @("-DSQLITE_THREADSAFE=0","-DSQLITE_OMIT_LOAD_EXTENSION") -OutName "sqlite3.o"
$mdbObj    = Get-CachedObj -Src (Join-Path $tp "mdb.c")     -Extra @() -OutName "mdb.o"
$midlObj   = Get-CachedObj -Src (Join-Path $tp "midl.c")    -Extra @() -OutName "midl.o"

gcc -O2 -std=c11 -DENGINE_YQ -I (Join-Path $proj "include") -o (Join-Path $bd "bench_yq.exe") (Join-Path $root "bench_kv.c") $objs
if ($LASTEXITCODE -ne 0) { throw "bench_yq build failed" }

gcc -O2 -std=c11 -DENGINE_SQLITE -I $tp -o (Join-Path $bd "bench_sqlite.exe") (Join-Path $root "bench_kv.c") $sqliteObj
if ($LASTEXITCODE -ne 0) { throw "bench_sqlite build failed" }

gcc -O2 -std=c11 -DENGINE_LMDB -I $tp -o (Join-Path $bd "bench_lmdb.exe") (Join-Path $root "bench_kv.c") $mdbObj $midlObj
if ($LASTEXITCODE -ne 0) { throw "bench_lmdb build failed" }

function Clean-Data {
    Get-ChildItem -Path $dat -Filter "yqbench*"  -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
    Get-ChildItem -Path $dat -Filter "sqlitebench*" -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
    Get-ChildItem -Path $dat -Filter "lmdbbench*" -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
}

function Size-Of($paths) {
    $sum = 0
    foreach ($p in $paths) {
        if (Test-Path $p) {
            $i = Get-Item $p
            if ($i.PSIsContainer) {
                $sum += (Get-ChildItem -Recurse -File $p | Measure-Object -Property Length -Sum).Sum
            } else {
                $sum += $i.Length
            }
        }
    }
    return [int64]$sum
}

$all = @()
foreach ($mode in ($Modes -split ",")) {
    Write-Output "=== mode: $mode ==="

    foreach ($order in ($Orders -split ",")) {
        Clean-Data
        $out = & (Join-Path $bd "bench_yq.exe") (Join-Path $dat "yqbench.yqdb") $N $mode $order
        $all += $out
        $all += "FILESIZE,yq-db,$mode,$order,$(Size-Of @((Join-Path $dat "yqbench.yqdb"), (Join-Path $dat "yqbench.yqdb.log")))"

        Clean-Data
        $out = & (Join-Path $bd "bench_sqlite.exe") (Join-Path $dat "sqlitebench.db") $N $mode $order
        $all += $out
        $all += "FILESIZE,sqlite,$mode,$order,$(Size-Of @((Join-Path $dat "sqlitebench.db"), (Join-Path $dat "sqlitebench.db-wal")))"

        Clean-Data
        New-Item -ItemType Directory -Force -Path (Join-Path $dat "lmdbbench") | Out-Null
        $lmdbArgs = @((Join-Path $dat "lmdbbench"), $N, $mode, $order)
        $out = & (Join-Path $bd "bench_lmdb.exe") @lmdbArgs
        if ($out | Select-String -SimpleMatch "OPEN_FAIL") {
            Start-Sleep -Milliseconds 800
            Clean-Data
            New-Item -ItemType Directory -Force -Path (Join-Path $dat "lmdbbench") | Out-Null
            $out = & (Join-Path $bd "bench_lmdb.exe") @lmdbArgs
        }
        $all += $out
        $all += "FILESIZE,lmdb,$mode,$order,$(Size-Of @((Join-Path $dat "lmdbbench")))"
    }
}

Clean-Data

Write-Output "=== raw results ==="
$all | ForEach-Object { Write-Output $_ }
