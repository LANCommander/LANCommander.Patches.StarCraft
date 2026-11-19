<#
.SYNOPSIS
    Shows or changes the StarCraft CD key stored in StarDat.mpq.
    Pure PowerShell replacement for SCKeyChanger.exe.

.DESCRIPTION
    The StarCraft installer (SCKeyChanger.exe is the same Blizzard installer engine running a cut-down script)
    stores the CD key inside StarDat.mpq:

        File in StarDat.mpq   Contents                    Encryption password
        -------------------   -------------------------   -------------------
        font\font.gid         CD key string               fixed (see $KeyFilePassword)
        font\font.clh         owner name string           fixed (see $KeyFilePassword)
        font\font.ccd         the text "sgubon"           the CD key
        font\font*.fnt        game fonts                  the CD key

    Because the fonts are encrypted with the key itself, changing the key means decrypting them with the old key
    and re-encrypting them with the new one. This script does that, writes the files back into a temporary copy of
    the archive, verifies the copy, and only then replaces the original (keeping StarDat.mpq.bak).

.EXAMPLE
    .\Set-StarCraftCDKey.ps1 -Show

.EXAMPLE
    .\Set-StarCraftCDKey.ps1 -CDKey 1234-56789-0123 -Owner "Jim Raynor"
#>
[CmdletBinding(DefaultParameterSetName = 'Set')]
param(
    # New 13-digit CD key. Dashes and spaces are ignored.
    [Parameter(ParameterSetName = 'Set', Mandatory = $true, Position = 0)]
    [string]$CDKey,

    # New owner name. Defaults to the name currently stored.
    [Parameter(ParameterSetName = 'Set')]
    [string]$Owner,

    # Only display the current key and owner.
    [Parameter(ParameterSetName = 'Show', Mandatory = $true)]
    [switch]$Show,

    [string]$StarCraftPath = 'C:\Games\StarCraft'
)

$ErrorActionPreference = 'Stop'

# Note: hex literals >= 0x80000000 need the L suffix, otherwise PowerShell parses them as negative Int32.
$UINT32_MASK = 0xFFFFFFFFL

# MPQ block flags
$MPQ_FILE_IMPLODE     = 0x00000100L
$MPQ_FILE_COMPRESS    = 0x00000200L
$MPQ_FILE_ENCRYPTED   = 0x00010000L
$MPQ_FILE_FIX_KEY     = 0x00020000L
$MPQ_FILE_SINGLE_UNIT = 0x01000000L
$MPQ_FILE_EXISTS      = 0x80000000L

# How rewritten files are stored: uncompressed, encrypted, key adjusted by block offset.
$MPQ_REWRITE_FLAGS = $MPQ_FILE_EXISTS -bor $MPQ_FILE_FIX_KEY -bor $MPQ_FILE_ENCRYPTED

# MPQ hash table sentinels
$HASH_ENTRY_EMPTY   = 0xFFFFFFFFL
$HASH_ENTRY_DELETED = 0xFFFFFFFEL

$KeyFile   = 'font\font.gid'
$OwnerFile = 'font\font.clh'
$FontFiles = @(
    'font\font.ccd'
    'font\font8.fnt', 'font\font10.fnt', 'font\font12.fnt', 'font\font14.fnt'
    'font\font16.fnt', 'font\font16x.fnt', 'font\font32.fnt', 'font\font50.fnt'
)

#region ---------------- General helpers ----------------

function Get-Slice([byte[]]$Bytes, [long]$Offset, [long]$Length) {
    $slice = New-Object 'byte[]' $Length
    [Array]::Copy($Bytes, $Offset, $slice, 0, $Length)
    , $slice
}

function ConvertTo-UInt32Array([byte[]]$Bytes) {
    $words = New-Object 'uint32[]' ([Math]::Floor($Bytes.Length / 4))
    [Buffer]::BlockCopy($Bytes, 0, $words, 0, $words.Length * 4)
    , $words
}

function ConvertFrom-UInt32Array([uint32[]]$Words) {
    $bytes = New-Object 'byte[]' ($Words.Length * 4)
    [Buffer]::BlockCopy($Words, 0, $bytes, 0, $bytes.Length)
    , $bytes
}

function Test-BytesEqual([byte[]]$A, [byte[]]$B) {
    [Convert]::ToBase64String($A) -eq [Convert]::ToBase64String($B)
}

# The installer works with NUL-terminated ANSI strings.
$Ansi = [Text.Encoding]::GetEncoding(28591)

function ConvertTo-CString([string]$Text) {
    , [byte[]]($Ansi.GetBytes($Text) + [byte]0)
}

function ConvertFrom-CString([byte[]]$Bytes) {
    $length = [Array]::IndexOf($Bytes, [byte]0)
    if ($length -lt 0) { $length = $Bytes.Length }
    $Ansi.GetString($Bytes, 0, $length)
}

function Read-StreamBytes([IO.Stream]$Stream, [long]$Position, [int]$Length) {
    $buffer = New-Object 'byte[]' $Length
    [void]$Stream.Seek($Position, 'Begin')
    $read = 0
    while ($read -lt $Length) {
        $n = $Stream.Read($buffer, $read, $Length - $read)
        if ($n -le 0) { throw 'Unexpected end of file' }
        $read += $n
    }
    , $buffer
}

function Write-StreamBytes([IO.Stream]$Stream, [long]$Position, [byte[]]$Bytes) {
    [void]$Stream.Seek($Position, 'Begin')
    $Stream.Write($Bytes, 0, $Bytes.Length)
}

#endregion

#region ---------------- MPQ archive (format version 0) ----------------

# Storm's 0x500-entry crypt table, shared by name hashing and the stream cipher.
$CryptTable = New-Object 'uint32[]' 0x500
& {
    [long]$seed = 0x00100001
    for ($index1 = 0; $index1 -lt 0x100; $index1++) {
        $index2 = $index1
        for ($i = 0; $i -lt 5; $i++) {
            $seed = ($seed * 125 + 3) % 0x2AAAAB
            $high = ($seed -band 0xFFFF) -shl 16
            $seed = ($seed * 125 + 3) % 0x2AAAAB
            $low = $seed -band 0xFFFF
            $CryptTable[$index2] = [uint32]($high -bor $low)
            $index2 += 0x100
        }
    }
}

# Hash types: 0 = table index, 1 = name check A, 2 = name check B, 3 = encryption key.
function Get-MpqHash([string]$Name, [int]$HashType) {
    [long]$seed1 = 0x7FED7FED
    [long]$seed2 = 0xEEEEEEEEL
    foreach ($char in $Name.ToUpperInvariant().ToCharArray()) {
        $c = [int]$char
        $seed1 = $CryptTable[($HashType -shl 8) + $c] -bxor (($seed1 + $seed2) -band $UINT32_MASK)
        $seed2 = ($c + $seed1 + $seed2 + ($seed2 -shl 5) + 3) -band $UINT32_MASK
    }
    [uint32]$seed1
}

# Storm stream cipher, applied in place to a uint32[].
function Invoke-MpqCrypt([uint32[]]$Words, [uint32]$Key, [bool]$Decrypt) {
    $M = $UINT32_MASK
    [long]$seed1 = $Key
    [long]$seed2 = 0xEEEEEEEEL
    for ($i = 0; $i -lt $Words.Length; $i++) {
        $seed2 = ($seed2 + $CryptTable[0x400 + ($seed1 -band 0xFF)]) -band $M
        [long]$word = $Words[$i]
        $output = $word -bxor (($seed1 + $seed2) -band $M)
        $Words[$i] = [uint32]$output
        $plain = if ($Decrypt) { $output } else { $word }
        # seed1 = ((~seed1 << 21) + 0x11111111) | (seed1 >> 11)
        $seed1 = ((((((-bnot $seed1) -band $M) -shl 21) -band $M) + 0x11111111) -band $M) -bor ($seed1 -shr 11)
        $seed2 = ($plain + $seed2 + ($seed2 -shl 5) + 3) -band $M
    }
}

# Stream cipher over a byte buffer. Only whole dwords are processed; trailing 1-3 bytes stay plain (as in Storm).
function Invoke-MpqCryptBytes([byte[]]$Bytes, [uint32]$Key, [bool]$Decrypt) {
    $words = ConvertTo-UInt32Array $Bytes
    Invoke-MpqCrypt $words $Key $Decrypt
    $result = [byte[]]$Bytes.Clone()
    [Buffer]::BlockCopy($words, 0, $result, 0, $words.Length * 4)
    , $result
}

# Each sector of a file is encrypted separately, with the file key + sector index.
function Invoke-MpqSectorCrypt([byte[]]$Data, [uint32]$Key, [int]$SectorSize, [bool]$Decrypt) {
    $result = New-Object 'byte[]' $Data.Length
    for ($sector = 0; $sector * $SectorSize -lt $Data.Length; $sector++) {
        $offset = $sector * $SectorSize
        $length = [Math]::Min($SectorSize, $Data.Length - $offset)
        $sectorKey = [uint32](($Key + $sector) -band $UINT32_MASK)
        $crypted = Invoke-MpqCryptBytes (Get-Slice $Data $offset $length) $sectorKey $Decrypt
        [Array]::Copy($crypted, 0, $result, $offset, $length)
    }
    , $result
}

function Read-MpqTable([IO.Stream]$Stream, [long]$Position, [int]$EntryCount, [string]$KeyName) {
    $words = ConvertTo-UInt32Array (Read-StreamBytes $Stream $Position ($EntryCount * 16))
    Invoke-MpqCrypt $words (Get-MpqHash $KeyName 3) $true
    , $words
}

function ConvertTo-MpqTableBytes([uint32[]]$Words, [string]$KeyName) {
    $copy = [uint32[]]$Words.Clone()
    Invoke-MpqCrypt $copy (Get-MpqHash $KeyName 3) $false
    , (ConvertFrom-UInt32Array $copy)
}

function Open-MpqArchive([string]$Path, [switch]$Writable) {
    $access = if ($Writable) { 'ReadWrite' } else { 'Read' }
    $stream = [IO.File]::Open($Path, 'Open', $access, 'Read')
    try {
        # The header ("MPQ\x1A") sits on a 512-byte boundary.
        $base = -1
        for ($pos = 0L; $pos + 32 -le $stream.Length; $pos += 0x200) {
            $magic = [BitConverter]::ToUInt32((Read-StreamBytes $stream $pos 4), 0)
            if ($magic -eq 0x1A51504D) { $base = $pos; break }
        }
        if ($base -lt 0) { throw "No MPQ header found in $Path" }

        $header = Read-StreamBytes $stream $base 32
        $archive = [pscustomobject]@{
            Path        = $Path
            Stream      = $stream
            Base        = $base
            ArchiveSize = [long][BitConverter]::ToUInt32($header, 8)
            SectorSize  = 0x200 -shl [BitConverter]::ToUInt16($header, 14)
            HashPos     = [long][BitConverter]::ToUInt32($header, 16)
            BlockPos    = [long][BitConverter]::ToUInt32($header, 20)
            HashCount   = [int][BitConverter]::ToUInt32($header, 24)
            BlockCount  = [int][BitConverter]::ToUInt32($header, 28)
            HashTable   = $null   # raw uint32[]: 4 words per entry (name A, name B, locale/platform, block index)
            Blocks      = $null   # parsed block table entries
        }

        $archive.HashTable = Read-MpqTable $stream ($base + $archive.HashPos) $archive.HashCount '(hash table)'
        $blockWords = Read-MpqTable $stream ($base + $archive.BlockPos) $archive.BlockCount '(block table)'
        $archive.Blocks = for ($i = 0; $i -lt $archive.BlockCount; $i++) {
            [pscustomobject]@{
                Offset         = [long]$blockWords[$i * 4]
                CompressedSize = [long]$blockWords[$i * 4 + 1]
                FileSize       = [long]$blockWords[$i * 4 + 2]
                Flags          = [long]$blockWords[$i * 4 + 3]
            }
        }
        $archive
    } catch {
        $stream.Close()
        throw
    }
}

function Close-MpqArchive($Archive) {
    if ($Archive) { $Archive.Stream.Close() }
}

# Returns the block table entry for $Name, or $null.
function Find-MpqBlock($Archive, [string]$Name) {
    $index = (Get-MpqHash $Name 0) % $Archive.HashCount
    $nameA = Get-MpqHash $Name 1
    $nameB = Get-MpqHash $Name 2
    for ($probe = 0; $probe -lt $Archive.HashCount; $probe++) {
        $entry = $index * 4
        $blockIndex = [long]$Archive.HashTable[$entry + 3]
        if ($blockIndex -eq $HASH_ENTRY_EMPTY) { return $null }
        if ($blockIndex -ne $HASH_ENTRY_DELETED -and
            $Archive.HashTable[$entry] -eq $nameA -and $Archive.HashTable[$entry + 1] -eq $nameB) {
            return $Archive.Blocks[$blockIndex]
        }
        $index = ($index + 1) % $Archive.HashCount
    }
    $null
}

# The file key comes from the file name without its path; FIX_KEY additionally mixes in offset and size.
function Get-MpqFileKey([string]$Name, [long]$Offset, [long]$FileSize, [long]$Flags) {
    [long]$key = Get-MpqHash ($Name -split '\\')[-1] 3
    if ($Flags -band $MPQ_FILE_FIX_KEY) {
        $key = (($key + $Offset) -bxor $FileSize) -band $UINT32_MASK
    }
    [uint32]$key
}

# --- PKWARE Data Compression Library "explode" (binary mode), used by Storm compression 0x08 ---

# Builds a decode table for a prefix code: indexed by the next $TableBits input bits (LSB first), it gives the
# symbol and the number of bits that symbol's code actually uses.
function New-DclDecodeTable([int[]]$CodeBits, [int[]]$Codes, [int]$TableBits) {
    $size = 1 -shl $TableBits
    $table = [pscustomobject]@{ Symbol = (New-Object 'int[]' $size); BitCount = (New-Object 'int[]' $size) }
    for ($symbol = 0; $symbol -lt $Codes.Length; $symbol++) {
        $bits = $CodeBits[$symbol]
        for ($index = $Codes[$symbol]; $index -lt $size; $index += 1 -shl $bits) {
            $table.Symbol[$index] = $symbol
            $table.BitCount[$index] = $bits
        }
    }
    $table
}

$DclLengthTable = New-DclDecodeTable `
    -CodeBits (3,2,3,3,4,4,4,5,5,5,5,6,6,6,7,7) `
    -Codes (5,3,1,6,10,2,12,20,4,24,8,48,16,32,64,0) `
    -TableBits 7
$DclDistanceTable = New-DclDecodeTable `
    -CodeBits (2,4,4,5,5,5,5,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8) `
    -Codes (0x03,0x0D,0x05,0x19,0x09,0x11,0x01,0x3E,0x1E,0x2E,0x0E,0x36,0x16,0x26,0x06,0x3A,0x1A,0x2A,0x0A,0x32,0x12,0x22,0x42,0x02,0x7C,0x3C,0x5C,0x1C,0x6C,0x2C,0x4C,0x0C,0x74,0x34,0x54,0x14,0x64,0x24,0x44,0x04,0x78,0x38,0x58,0x18,0x68,0x28,0x48,0x08,0xF0,0x70,0xB0,0x30,0xD0,0x50,0x90,0x10,0xE0,0x60,0xA0,0x20,0xC0,0x40,0x80,0x00) `
    -TableBits 8
$DclLengthExtraBits = [int[]](0,0,0,0,0,0,0,0,1,2,3,4,5,6,7,8)
$DclLengthBase      = [int[]](0,1,2,3,4,5,6,7,8,10,14,22,38,70,134,262)

# Input is a bit stream read LSB first. Bit reading is inlined (no helper calls) because this loop runs once per
# output token. Each "top up" loads bytes until the buffer holds at least 16 bits, which is enough for the next step:
#   flag (1) + literal (8)  or  flag (1) + length code (<= 7) + length extra bits (<= 8)
#   distance code (<= 8) + distance low bits (<= 6)
function Expand-Pkware([byte[]]$Source, [int]$OutputSize) {
    if ($Source[0] -ne 0) { throw 'PKWARE ASCII mode is not supported' }
    $dictionaryBits = $Source[1]
    $output = New-Object 'byte[]' $OutputSize
    $outPos = 0
    $inPos = 2
    [long]$bitBuffer = 0
    $bitCount = 0

    while ($outPos -lt $OutputSize) {
        while ($bitCount -lt 16) {   # top up
            $next = if ($inPos -lt $Source.Length) { $Source[$inPos] } else { 0 }
            $bitBuffer = $bitBuffer -bor ([long]$next -shl $bitCount); $inPos++; $bitCount += 8
        }

        $isMatch = $bitBuffer -band 1
        $bitBuffer = $bitBuffer -shr 1; $bitCount -= 1
        if (-not $isMatch) {
            $output[$outPos++] = [byte]($bitBuffer -band 0xFF)   # literal byte
            $bitBuffer = $bitBuffer -shr 8; $bitCount -= 8
            continue
        }

        # Match length: prefix code, then extra bits.
        $index = $bitBuffer -band 0x7F
        $lengthSymbol = $DclLengthTable.Symbol[$index]
        $used = $DclLengthTable.BitCount[$index]
        $bitBuffer = $bitBuffer -shr $used; $bitCount -= $used

        $extraBits = $DclLengthExtraBits[$lengthSymbol]
        $extra = [int]($bitBuffer -band ((1 -shl $extraBits) - 1))
        $bitBuffer = $bitBuffer -shr $extraBits; $bitCount -= $extraBits

        $length = $DclLengthBase[$lengthSymbol] + $extra + 2
        if ($length -eq 519) { break }                           # end-of-stream marker

        while ($bitCount -lt 16) {   # top up
            $next = if ($inPos -lt $Source.Length) { $Source[$inPos] } else { 0 }
            $bitBuffer = $bitBuffer -bor ([long]$next -shl $bitCount); $inPos++; $bitCount += 8
        }

        # Match distance: prefix code for the high bits, then 2 (for length 2) or dictionaryBits low bits.
        $index = $bitBuffer -band 0xFF
        $distanceSymbol = $DclDistanceTable.Symbol[$index]
        $used = $DclDistanceTable.BitCount[$index]
        $bitBuffer = $bitBuffer -shr $used; $bitCount -= $used

        $lowBits = if ($length -eq 2) { 2 } else { $dictionaryBits }
        $low = [int]($bitBuffer -band ((1 -shl $lowBits) - 1))
        $bitBuffer = $bitBuffer -shr $lowBits; $bitCount -= $lowBits

        $distance = ($distanceSymbol -shl $lowBits) + $low + 1
        for ($i = 0; $i -lt $length -and $outPos -lt $OutputSize; $i++) {
            $output[$outPos] = $output[$outPos - $distance]
            $outPos++
        }
    }
    , $output
}

function Expand-MpqSector([byte[]]$Data, [int]$OutputSize, [long]$Flags) {
    if ($Data.Length -ge $OutputSize) { return , $Data }      # sector was stored uncompressed
    if ($Flags -band $MPQ_FILE_IMPLODE) { return , (Expand-Pkware $Data $OutputSize) }

    # MPQ_FILE_COMPRESS: first byte says which compressor was used.
    $method = $Data[0]
    $payload = Get-Slice $Data 1 ($Data.Length - 1)
    switch ($method) {
        0x08 { return , (Expand-Pkware $payload $OutputSize) }
        0x02 {
            # zlib: skip the 2-byte header and inflate.
            $memory = New-Object IO.MemoryStream(, $payload)
            [void]$memory.Seek(2, 'Begin')
            $inflate = New-Object IO.Compression.DeflateStream($memory, [IO.Compression.CompressionMode]::Decompress)
            $output = New-Object 'byte[]' $OutputSize
            $read = 0
            while ($read -lt $OutputSize) {
                $n = $inflate.Read($output, $read, $OutputSize - $read)
                if ($n -le 0) { break }
                $read += $n
            }
            return , $output
        }
        default { throw ('Unsupported MPQ compression method 0x{0:X2}' -f $method) }
    }
}

function Read-MpqFile($Archive, [string]$Name) {
    $block = Find-MpqBlock $Archive $Name
    if (-not $block) { throw "File not found in $($Archive.Path): $Name" }

    $raw = Read-StreamBytes $Archive.Stream ($Archive.Base + $block.Offset) $block.CompressedSize
    $encrypted = ($block.Flags -band $MPQ_FILE_ENCRYPTED) -ne 0
    $key = if ($encrypted) { Get-MpqFileKey $Name $block.Offset $block.FileSize $block.Flags } else { 0 }
    $sectorSize = $Archive.SectorSize

    # Uncompressed file
    if (($block.Flags -band ($MPQ_FILE_IMPLODE -bor $MPQ_FILE_COMPRESS)) -eq 0) {
        if (-not $encrypted) { return , $raw }
        if ($block.Flags -band $MPQ_FILE_SINGLE_UNIT) { return , (Invoke-MpqCryptBytes $raw $key $true) }
        return , (Invoke-MpqSectorCrypt $raw $key $sectorSize $true)
    }

    # Compressed file: a sector offset table (one dword per sector + end) precedes the sectors.
    $sectorCount = [int][Math]::Ceiling($block.FileSize / $sectorSize)
    $offsets = ConvertTo-UInt32Array (Get-Slice $raw 0 (($sectorCount + 1) * 4))
    if ($encrypted) { Invoke-MpqCrypt $offsets ([uint32](($key - 1) -band $UINT32_MASK)) $true }

    $output = New-Object 'byte[]' $block.FileSize
    for ($sector = 0; $sector -lt $sectorCount; $sector++) {
        $data = Get-Slice $raw $offsets[$sector] ($offsets[$sector + 1] - $offsets[$sector])
        if ($encrypted) { $data = Invoke-MpqCryptBytes $data ([uint32](($key + $sector) -band $UINT32_MASK)) $true }
        $expectedSize = [Math]::Min($sectorSize, $block.FileSize - $sector * $sectorSize)
        $plain = Expand-MpqSector $data $expectedSize $block.Flags
        [Array]::Copy($plain, 0, $output, $sector * $sectorSize, $expectedSize)
    }
    , $output
}

# Replaces the contents of existing files ($Files: name -> byte[]), storing them uncompressed and encrypted.
# A file already stored that way with the same size is overwritten in place. Otherwise its data is appended after
# the last block and the hash/block tables are moved after it.
function Write-MpqFiles($Archive, [Collections.IDictionary]$Files) {
    $stream = $Archive.Stream
    $appendOffset = [long]($Archive.Blocks | ForEach-Object { $_.Offset + $_.CompressedSize } | Measure-Object -Maximum).Maximum
    $appended = $false

    foreach ($name in $Files.Keys) {
        $data = [byte[]]$Files[$name]
        $block = Find-MpqBlock $Archive $name
        if (-not $block) { throw "File not found in archive: $name" }

        $canOverwrite = ($block.Flags -band ($MPQ_FILE_IMPLODE -bor $MPQ_FILE_COMPRESS -bor $MPQ_FILE_SINGLE_UNIT)) -eq 0 -and
                        $block.CompressedSize -eq $data.Length -and $block.FileSize -eq $data.Length
        if (-not $canOverwrite) {
            $block.Offset = $appendOffset
            $appendOffset += $data.Length
            $appended = $true
        }
        $block.CompressedSize = $data.Length
        $block.FileSize = $data.Length
        $block.Flags = $MPQ_REWRITE_FLAGS

        $key = Get-MpqFileKey $name $block.Offset $block.FileSize $block.Flags
        Write-StreamBytes $stream ($Archive.Base + $block.Offset) (Invoke-MpqSectorCrypt $data $key $Archive.SectorSize $false)
    }

    $hashTableSize = $Archive.HashCount * 16
    $blockTableSize = $Archive.BlockCount * 16
    if ($appended) {
        $Archive.HashPos = $appendOffset
        $Archive.BlockPos = $appendOffset + $hashTableSize
        $Archive.ArchiveSize = $Archive.BlockPos + $blockTableSize
        $stream.SetLength($Archive.Base + $Archive.ArchiveSize)
    }

    $blockWords = New-Object 'uint32[]' ($Archive.BlockCount * 4)
    for ($i = 0; $i -lt $Archive.BlockCount; $i++) {
        $b = $Archive.Blocks[$i]
        $blockWords[$i * 4] = [uint32]$b.Offset
        $blockWords[$i * 4 + 1] = [uint32]$b.CompressedSize
        $blockWords[$i * 4 + 2] = [uint32]$b.FileSize
        $blockWords[$i * 4 + 3] = [uint32]$b.Flags
    }
    Write-StreamBytes $stream ($Archive.Base + $Archive.HashPos) (ConvertTo-MpqTableBytes $Archive.HashTable '(hash table)')
    Write-StreamBytes $stream ($Archive.Base + $Archive.BlockPos) (ConvertTo-MpqTableBytes $blockWords '(block table)')

    Write-StreamBytes $stream ($Archive.Base + 8) ([BitConverter]::GetBytes([uint32]$Archive.ArchiveSize))
    Write-StreamBytes $stream ($Archive.Base + 16) ([BitConverter]::GetBytes([uint32]$Archive.HashPos))
    Write-StreamBytes $stream ($Archive.Base + 20) ([BitConverter]::GetBytes([uint32]$Archive.BlockPos))
    $stream.Flush()
}

#endregion

#region ---------------- Installer "EncryptKey" / "EncryptFile" cipher ----------------
#
# Reversed from SCKeyChanger.exe (FUN_004115b0, FUN_004116d0, FUN_004118b0).
#
# Setup, from a password:
#   R      = 112 bytes of MSVC rand() after srand(0x4FA7)
#   digest = one SHA compression of the password repeated to fill 64 bytes
#   R[i]  ^= digest[i % 20]
#   IDEA key = R[0..15]; running hash state = one SHA compression of R[48..111]
#
# Encryption, per 64-byte chunk n (the last chunk is zero-padded):
#   mask = current running hash state (20 bytes)
#   if n % 16 == 0: compress the plaintext chunk into the running hash state
#   chunk[i] += mask[(63 - i) % 20]
#   if n % 8 == 0: IDEA-encrypt the chunk (8 blocks, ECB)
#
# Trailer (8 bytes): running hash word 0, 0x00, length of the last chunk, 0x00, 0x00.
#
# "SHA" is Blizzard's broken SHA-1: words are little-endian, the message schedule has no rotate, the rotates
# use an arithmetic shift, and no length padding is ever applied. IDEA also loads its 16-bit words little-endian.

function Get-MsvcRandBytes([int]$Seed, [int]$Count, [switch]$SkipZero) {
    [long]$state = $Seed
    $bytes = New-Object System.Collections.Generic.List[byte]
    while ($bytes.Count -lt $Count) {
        $state = ($state * 0x343FD + 0x269EC3) -band $UINT32_MASK
        $value = [byte](($state -shr 16) -band 0xFF)       # low byte of rand()
        if ($SkipZero -and $value -eq 0) { continue }
        $bytes.Add($value)
    }
    , $bytes.ToArray()
}

# Password for the key and owner files: srand(0x150B), then 19 non-zero rand() bytes.
$KeyFilePassword = Get-MsvcRandBytes 0x150B 19 -SkipZero

function New-BrokenShaState {
    , [long[]](0x67452301L, 0xEFCDAB89L, 0x98BADCFEL, 0x10325476L, 0xC3D2E1F0L)
}

function Invoke-BrokenShaBlock([long[]]$State, [byte[]]$Block) {
    $M = $UINT32_MASK
    $w = New-Object 'long[]' 80
    for ($i = 0; $i -lt 16; $i++) { $w[$i] = [long][BitConverter]::ToUInt32($Block, $i * 4) }
    for ($i = 16; $i -lt 80; $i++) { $w[$i] = $w[$i - 3] -bxor $w[$i - 8] -bxor $w[$i - 14] -bxor $w[$i - 16] }

    # Blizzard's "rotate left" is (x << n) | (x >> (32 - n)) on a *signed* int, so the right shift sign-extends.
    # Below, ($x - (($x -band 0x80000000L) -shl 1)) is x reinterpreted as a signed value; -shr on it is arithmetic.
    $a, $b, $c, $d, $e = $State
    for ($i = 0; $i -lt 80; $i++) {
        if ($i -lt 20)     { $f = (($M -bxor $b) -band $d) -bor ($b -band $c); $constant = 0x5A827999L }
        elseif ($i -lt 40) { $f = $b -bxor $c -bxor $d;                        $constant = 0x6ED9EBA1L }
        elseif ($i -lt 60) { $f = (($b -bor $c) -band $d) -bor ($b -band $c);  $constant = 0x8F1BBCDCL }
        else               { $f = $b -bxor $c -bxor $d;                        $constant = 0xCA62C1D6L }

        $rotatedA = (($a -shl 5) -band $M) -bor ((($a - (($a -band 0x80000000L) -shl 1)) -shr 27) -band $M)
        $rotatedB = (($b -shl 30) -band $M) -bor ((($b - (($b -band 0x80000000L) -shl 1)) -shr 2) -band $M)

        $temp = ($rotatedA + $f + $w[$i] + $constant + $e) -band $M
        $e = $d
        $d = $c
        $c = $rotatedB
        $b = $a
        $a = $temp
    }
    $State[0] = ($State[0] + $a) -band $M
    $State[1] = ($State[1] + $b) -band $M
    $State[2] = ($State[2] + $c) -band $M
    $State[3] = ($State[3] + $d) -band $M
    $State[4] = ($State[4] + $e) -band $M
}

function ConvertFrom-BrokenShaState([long[]]$State) {
    $bytes = New-Object 'byte[]' 20
    for ($i = 0; $i -lt 5; $i++) {
        [Array]::Copy([BitConverter]::GetBytes([uint32]$State[$i]), 0, $bytes, $i * 4, 4)
    }
    , $bytes
}

# IDEA multiplication modulo 65537, where 0 stands for 65536.
function Invoke-IdeaMultiply([int]$A, [int]$B) {
    if ($A -eq 0) { return (1 - $B) -band 0xFFFF }
    if ($B -eq 0) { return (1 - $A) -band 0xFFFF }
    [long]$product = [long]$A * $B
    $low = [int]($product -band 0xFFFF)
    $high = [int]($product -shr 16)
    ($low - $high + [int]($low -lt $high)) -band 0xFFFF
}

# Multiplicative inverse modulo 65537 (extended Euclid).
function Get-IdeaInverse([int]$X) {
    if ($X -le 1) { return $X }
    [long]$r0 = 0x10001; [long]$r1 = $X
    [long]$t0 = 0;       [long]$t1 = 1
    while ($r1 -ne 0) {
        $quotient = [long][Math]::Floor($r0 / $r1)
        $r0, $r1 = $r1, ($r0 - $quotient * $r1)
        $t0, $t1 = $t1, ($t0 - $quotient * $t1)
    }
    [int](($t0 % 0x10001 + 0x10001) % 0x10001) -band 0xFFFF
}

function Get-IdeaNegative([int]$X) { (-$X) -band 0xFFFF }

# 52 encryption subkeys: the 128-bit key, then repeated 25-bit rotations of it.
function New-IdeaEncryptionKey([byte[]]$Key) {
    $subkeys = New-Object 'int[]' 52
    for ($i = 0; $i -lt 8; $i++) { $subkeys[$i] = [BitConverter]::ToUInt16($Key, $i * 2) }
    for ($j = 8; $j -lt 52; $j++) {
        $previousGroup = $j - ($j % 8) - 8
        $i = $j % 8
        $hi = $subkeys[$previousGroup + (($i + 1) % 8)]
        $lo = $subkeys[$previousGroup + (($i + 2) % 8)]
        $subkeys[$j] = (($hi -shl 9) -bor ($lo -shr 7)) -band 0xFFFF
    }
    , $subkeys
}

# Decryption subkeys: the encryption subkeys inverted and in reverse round order.
function New-IdeaDecryptionKey([int[]]$Enc) {
    $dec = New-Object 'int[]' 52
    for ($round = 0; $round -le 8; $round++) {
        $src = $round * 6
        $dst = 48 - $round * 6
        # The middle rounds swap the two additive subkeys; the first and last do not.
        $swap = $round -gt 0 -and $round -lt 8
        $dec[$dst]     = Get-IdeaInverse $Enc[$src]
        $dec[$dst + 1] = Get-IdeaNegative $Enc[$src + ($(if ($swap) { 2 } else { 1 }))]
        $dec[$dst + 2] = Get-IdeaNegative $Enc[$src + ($(if ($swap) { 1 } else { 2 }))]
        $dec[$dst + 3] = Get-IdeaInverse $Enc[$src + 3]
        if ($round -lt 8) {
            $dec[$dst - 2] = $Enc[$src + 4]
            $dec[$dst - 1] = $Enc[$src + 5]
        }
    }
    , $dec
}

# IDEA on the 8 bytes at $Offset, in place. Encrypts or decrypts depending on the subkeys given.
function Invoke-IdeaBlock([byte[]]$Buffer, [int]$Offset, [int[]]$Subkeys) {
    $x1 = [int][BitConverter]::ToUInt16($Buffer, $Offset)
    $x2 = [int][BitConverter]::ToUInt16($Buffer, $Offset + 2)
    $x3 = [int][BitConverter]::ToUInt16($Buffer, $Offset + 4)
    $x4 = [int][BitConverter]::ToUInt16($Buffer, $Offset + 6)

    for ($n = 0; $n -lt 48; $n += 6) {
        $x1 = Invoke-IdeaMultiply $x1 $Subkeys[$n]
        $x2 = ($x2 + $Subkeys[$n + 1]) -band 0xFFFF
        $x3 = ($x3 + $Subkeys[$n + 2]) -band 0xFFFF
        $x4 = Invoke-IdeaMultiply $x4 $Subkeys[$n + 3]

        $t = Invoke-IdeaMultiply ($x1 -bxor $x3) $Subkeys[$n + 4]
        $u = Invoke-IdeaMultiply ((($x2 -bxor $x4) + $t) -band 0xFFFF) $Subkeys[$n + 5]
        $t = ($t + $u) -band 0xFFFF

        $x1 = $x1 -bxor $u
        $x4 = $x4 -bxor $t
        $x2, $x3 = ($x3 -bxor $u), ($x2 -bxor $t)        # the middle words swap every round
    }

    $result = @(
        (Invoke-IdeaMultiply $x1 $Subkeys[48])
        (($x3 + $Subkeys[49]) -band 0xFFFF)
        (($x2 + $Subkeys[50]) -band 0xFFFF)
        (Invoke-IdeaMultiply $x4 $Subkeys[51])
    )
    for ($i = 0; $i -lt 4; $i++) {
        $Buffer[$Offset + 2 * $i] = [byte]($result[$i] -band 0xFF)
        $Buffer[$Offset + 2 * $i + 1] = [byte]($result[$i] -shr 8)
    }
}

# Returns the IDEA encryption subkeys and the initial running hash state for a password.
function Initialize-InstallerCipher([byte[]]$Password) {
    # Password repeated to 64 bytes (an empty password gives all zeros).
    $passwordBlock = New-Object 'byte[]' 64
    if ($Password.Length) {
        for ($i = 0; $i -lt 64; $i++) { $passwordBlock[$i] = $Password[$i % $Password.Length] }
    }
    $digestState = New-BrokenShaState
    Invoke-BrokenShaBlock $digestState $passwordBlock
    $digest = ConvertFrom-BrokenShaState $digestState

    $material = Get-MsvcRandBytes 0x4FA7 112
    for ($i = 0; $i -lt $material.Length; $i++) { $material[$i] = $material[$i] -bxor $digest[$i % 20] }

    $runningState = New-BrokenShaState
    Invoke-BrokenShaBlock $runningState (Get-Slice $material 48 64)

    [pscustomobject]@{
        IdeaKey = New-IdeaEncryptionKey (Get-Slice $material 0 16)
        State   = $runningState
    }
}

function Protect-InstallerData([byte[]]$Data, [byte[]]$Password) {
    $cipher = Initialize-InstallerCipher $Password
    $bodySize = [int][Math]::Ceiling($Data.Length / 64) * 64
    $output = New-Object 'byte[]' ($bodySize + 8)
    $lastChunkLength = 0

    for ($chunk = 0; $chunk * 64 -lt $Data.Length; $chunk++) {
        $offset = $chunk * 64
        $lastChunkLength = [Math]::Min(64, $Data.Length - $offset)
        $block = New-Object 'byte[]' 64
        [Array]::Copy($Data, $offset, $block, 0, $lastChunkLength)

        $mask = ConvertFrom-BrokenShaState $cipher.State
        if ($chunk % 16 -eq 0) { Invoke-BrokenShaBlock $cipher.State $block }
        for ($i = 0; $i -lt 64; $i++) { $block[$i] = [byte](($block[$i] + $mask[(63 - $i) % 20]) -band 0xFF) }
        if ($chunk % 8 -eq 0) {
            for ($i = 0; $i -lt 64; $i += 8) { Invoke-IdeaBlock $block $i $cipher.IdeaKey }
        }
        [Array]::Copy($block, 0, $output, $offset, 64)
    }

    [Array]::Copy([BitConverter]::GetBytes([uint32]$cipher.State[0]), 0, $output, $bodySize, 4)
    $output[$bodySize + 5] = [byte]$lastChunkLength
    , $output
}

# Returns Data (the plaintext) and Valid (whether the trailer's hash word matches).
function Unprotect-InstallerData([byte[]]$Encrypted, [byte[]]$Password) {
    $bodySize = $Encrypted.Length - 8
    if ($bodySize -lt 64 -or $bodySize % 64 -ne 0) { return [pscustomobject]@{ Data = $null; Valid = $false } }
    $lastChunkLength = $Encrypted[$bodySize + 5]
    if ($lastChunkLength -eq 0 -or $lastChunkLength -gt 64) { $lastChunkLength = 64 }

    $cipher = Initialize-InstallerCipher $Password
    $ideaDecryptKey = New-IdeaDecryptionKey $cipher.IdeaKey
    $output = New-Object 'byte[]' ($bodySize - 64 + $lastChunkLength)

    for ($chunk = 0; $chunk * 64 -lt $bodySize; $chunk++) {
        $offset = $chunk * 64
        $block = Get-Slice $Encrypted $offset 64

        $mask = ConvertFrom-BrokenShaState $cipher.State
        if ($chunk % 8 -eq 0) {
            for ($i = 0; $i -lt 64; $i += 8) { Invoke-IdeaBlock $block $i $ideaDecryptKey }
        }
        for ($i = 0; $i -lt 64; $i++) { $block[$i] = [byte](($block[$i] - $mask[(63 - $i) % 20]) -band 0xFF) }

        $length = if ($offset + 64 -ge $bodySize) { $lastChunkLength } else { 64 }
        for ($i = $length; $i -lt 64; $i++) { $block[$i] = 0 }   # the encryptor hashed the zero-padded chunk
        if ($chunk % 16 -eq 0) { Invoke-BrokenShaBlock $cipher.State $block }
        [Array]::Copy($block, 0, $output, $offset, $length)
    }

    [pscustomobject]@{
        Data  = $output
        Valid = [BitConverter]::ToUInt32($Encrypted, $bodySize) -eq [uint32]$cipher.State[0]
    }
}

#endregion

#region ---------------- StarCraft key handling ----------------

# Same check as the installer (FUN_00411160): 13 digits, the last one is a check digit.
function Test-StarCraftCDKey([string]$Key) {
    if ($Key -notmatch '^\d{13}$') { return $false }
    [long]$value = 3
    for ($i = 0; $i -lt 12; $i++) {
        $digit = [int][char]$Key[$i] - 48
        $value = ($value + ($digit -bxor (($value * 2) -band $UINT32_MASK))) -band $UINT32_MASK
    }
    ($value % 10) -eq ([int][char]$Key[12] - 48)
}

function Read-InstallerString($Archive, [string]$Name) {
    $result = Unprotect-InstallerData (Read-MpqFile $Archive $Name) $KeyFilePassword
    [pscustomobject]@{
        Text  = if ($result.Data) { ConvertFrom-CString $result.Data } else { $null }
        Valid = $result.Valid
    }
}

function Get-StarCraftKeyInfo($Archive) {
    $key = Read-InstallerString $Archive $KeyFile
    $owner = Read-InstallerString $Archive $OwnerFile
    [pscustomobject]@{
        CDKey      = $key.Text
        KeyValid   = $key.Valid
        Owner      = $owner.Text
        OwnerValid = $owner.Valid
    }
}

# Builds the encrypted replacement files for the new key. Returns the files and the font plaintexts.
function New-RekeyedFiles($Archive, [string]$OldKey, [string]$NewKey, [string]$NewOwner) {
    $oldPassword = $Ansi.GetBytes($OldKey)
    $newPassword = $Ansi.GetBytes($NewKey)

    $files = [ordered]@{}
    $files[$KeyFile] = Protect-InstallerData (ConvertTo-CString $NewKey) $KeyFilePassword
    $files[$OwnerFile] = Protect-InstallerData (ConvertTo-CString $NewOwner) $KeyFilePassword

    $fontPlaintext = @{}
    foreach ($font in $FontFiles) {
        Write-Host "Re-encrypting $font"
        $decrypted = Unprotect-InstallerData (Read-MpqFile $Archive $font) $oldPassword
        if (-not $decrypted.Valid) { throw "$font did not decrypt with the current key (checksum mismatch)." }
        $fontPlaintext[$font] = $decrypted.Data
        $files[$font] = Protect-InstallerData $decrypted.Data $newPassword
    }
    [pscustomobject]@{ Files = $files; FontPlaintext = $fontPlaintext }
}

function Assert-RekeyedArchive([string]$Path, [string]$Key, [string]$Owner, [hashtable]$FontPlaintext) {
    $archive = Open-MpqArchive $Path
    try {
        $info = Get-StarCraftKeyInfo $archive
        if (-not $info.KeyValid -or $info.CDKey -ne $Key) { throw "Verification failed: $KeyFile" }
        if (-not $info.OwnerValid -or $info.Owner -ne $Owner) { throw "Verification failed: $OwnerFile" }

        $password = $Ansi.GetBytes($Key)
        foreach ($font in $FontFiles) {
            $decrypted = Unprotect-InstallerData (Read-MpqFile $archive $font) $password
            if (-not $decrypted.Valid -or -not (Test-BytesEqual $decrypted.Data $FontPlaintext[$font])) {
                throw "Verification failed: $font"
            }
        }
    } finally {
        Close-MpqArchive $archive
    }
}

#endregion

#region ---------------- Main ----------------

$mpqPath = Join-Path $StarCraftPath 'StarDat.mpq'
if (-not (Test-Path -LiteralPath $mpqPath)) { throw "StarDat.mpq not found at $mpqPath" }

if ($Show) {
    $archive = Open-MpqArchive $mpqPath
    try { $info = Get-StarCraftKeyInfo $archive } finally { Close-MpqArchive $archive }
    $info | Add-Member -NotePropertyName ChecksumOK -NotePropertyValue (Test-StarCraftCDKey $info.CDKey) -PassThru
    return
}

$newKey = $CDKey -replace '[\s-]', ''
if (-not (Test-StarCraftCDKey $newKey)) {
    throw "'$CDKey' is not a valid StarCraft CD key (13 digits with a valid check digit)."
}

# 1. Read the current key and build the re-encrypted files.
$archive = Open-MpqArchive $mpqPath
try {
    $current = Get-StarCraftKeyInfo $archive
    if (-not $current.KeyValid) { throw "$KeyFile could not be decrypted; cannot recover the current key." }
    if (-not $PSBoundParameters.ContainsKey('Owner')) { $Owner = $current.Owner }

    Write-Host "Current key : $($current.CDKey)  (owner '$($current.Owner)')"
    Write-Host "New key     : $newKey  (owner '$Owner')"
    $rekeyed = New-RekeyedFiles $archive $current.CDKey $newKey $Owner
} finally {
    Close-MpqArchive $archive
}

# 2. Write them into a temporary copy, verify it, then swap it in.
$tempPath = "$mpqPath.tmp"
$backupPath = "$mpqPath.bak"
Copy-Item -LiteralPath $mpqPath -Destination $tempPath -Force
try {
    $archive = Open-MpqArchive $tempPath -Writable
    try { Write-MpqFiles $archive $rekeyed.Files } finally { Close-MpqArchive $archive }

    Write-Host 'Verifying...'
    Assert-RekeyedArchive $tempPath $newKey $Owner $rekeyed.FontPlaintext

    if (-not (Test-Path -LiteralPath $backupPath)) {
        Copy-Item -LiteralPath $mpqPath -Destination $backupPath
        Write-Host "Backup saved to $backupPath"
    }
    Move-Item -LiteralPath $tempPath -Destination $mpqPath -Force
    Write-Host "CD key changed to $newKey."
} catch {
    Remove-Item -LiteralPath $tempPath -Force -ErrorAction SilentlyContinue
    throw
}

#endregion
