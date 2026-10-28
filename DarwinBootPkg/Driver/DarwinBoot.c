// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Guid/FileInfo.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiLib.h>
#include <Protocol/UniXNULoader.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimpleFileSystem.h>
#include "BootArgs.h"
#include "BootVideo.h"
#include "PackedRamdisk.h"
#include "WindowSize.h"

#define WINDOW_SIZE  SIZE_256MB
#define BLOCK_SIZE   SIZE_2MB

//
// The top of the low window this loader allocates from.
//
// A page placed above this was seen to break a following read, so ReadFile keeps its
// buffers below DRAM_TOP and starts them DRAM_TOP - size lower still. That is a policy of
// this loader, chosen for a window that was observed to work; it is not a measurement of
// all the RAM the device has, and it does not override the firmware's memory map, which
// remains the authority on what is actually memory.
//
#define DRAM_TOP     0xBC000000U
#define MAX_SEGMENTS 64

typedef struct {
  UINT64  Address;
  UINT64  Size;
  UINT64  FileOffset;
  UINT64  FileSize;
  UINT32  Protection;
} SEGMENT;

typedef struct {
  SEGMENT  Segments[MAX_SEGMENTS];
  UINTN    Count;
  UINT64   Base;
  UINT64   End;
  UINT64   Entry;
} KERNEL;

STATIC EFI_HANDLE  mImage;
UINT64 EFIAPI DarwinCurrentEl (VOID);
VOID EFIAPI DarwinEnter (UINT64 Args, UINT64 Entry, UINT64 Memory, UINT64 Size, UINT64 VideoBase, UINT64 VideoBytes);

#define READ_SYSREG(Name)  ({ UINT64 Value_; __asm__ volatile ("mrs %0, " #Name : "=r" (Value_)); Value_; })

//
// Progress the eye can see.
//
// Print() writes to the text console, and on this board that console is not the surface UniXNU
// paints: lines appear there only by accident, and the screen keeps showing an older frame, so
// a boot that stalls after "getinfo" looks exactly like one that never printed it. OutputString
// on gST->ConOut reaches the same console the menu text uses, and each line is preceded by a
// form feed, which the terminal moves the cursor with rather than scrolling behind. A form feed
// is a request, not a guarantee: a terminal that ignores it leaves the earlier frame partly in
// view, and ConOut itself may have been left stale by an earlier user of the console.
//
STATIC
VOID
Band (
  IN CONST CHAR16  *Line
  )
{
  if ((gST == NULL) || (gST->ConOut == NULL) || (Line == NULL)) {
    return;
  }

  gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)L"\f");
  gST->ConOut->OutputString (gST->ConOut, (CHAR16 *)Line);
}

STATIC
VOID
BandInit (
  VOID
  )
{
}

STATIC UINT32 U32 (CONST VOID *P) { return ReadUnaligned32 ((CONST UINT32 *)P); }
STATIC UINT64 U64 (CONST VOID *P) { return ReadUnaligned64 ((CONST UINT64 *)P); }

STATIC
EFI_STATUS
ReadBootVideo (UINT64 Video[6])
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL           *Gop;
  EFI_STATUS                            Status;

  ZeroMem (Video, 6 * sizeof (*Video));
  Status = gBS->HandleProtocol (gST->ConsoleOutHandle, &gEfiGraphicsOutputProtocolGuid, (VOID **)&Gop);
  if (EFI_ERROR (Status)) {
    Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  }
  if (Status == EFI_NOT_FOUND) { return EFI_SUCCESS; }
  if (EFI_ERROR (Status)) { return Status; }
  return DarwinBootVideoFromGop (Gop, Video);
}

STATIC
EFI_STATUS
ReadFile (
  EFI_FILE_PROTOCOL *Root,
  CONST CHAR16      *Path,
  UINTN             Limit,
  VOID              **Data,
  UINTN             *Size
  )
{
  EFI_STATUS         Status;
  EFI_FILE_PROTOCOL  *File;
  EFI_FILE_INFO      *Info;
  UINTN              InfoSize;
  UINTN              Actual;
  UINTN              AllocatedSize;
  BOOLEAN            PagesAllocated;

  if ((Root == NULL) || (Path == NULL) || (Data == NULL) || (Size == NULL)) { return EFI_INVALID_PARAMETER; }
  *Data = NULL;
  *Size = 0;
  File = NULL;
  Info = NULL;
  AllocatedSize = 0;
  PagesAllocated = FALSE;
  Band (L"DB01 open");
  Status = Root->Open (Root, &File, (CHAR16 *)Path, EFI_FILE_MODE_READ, 0);
  { CHAR16 T[128]; UnicodeSPrint (T, sizeof (T), L"DB02 opened %r file %p path %s",
                                  Status, File, Path); Band (T); }
  if (EFI_ERROR (Status)) { return Status; }
  if (File == NULL) {
    //
    // A reader that reports success and hands back nothing cannot be used; the close
    // in Done is guarded on File for the same reason.
    //
    Band (L"DB02a opened without a file");
    return EFI_DEVICE_ERROR;
  }
  Info = AllocatePages (EFI_SIZE_TO_PAGES (SIZE_OF_EFI_FILE_INFO + 512));
  if (Info == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }
  InfoSize = SIZE_OF_EFI_FILE_INFO + 512;
  Band (L"DB03 getinfo");
  Status = File->GetInfo (File, &gEfiFileInfoGuid, &InfoSize, Info);
  { CHAR16 T[128]; UnicodeSPrint (T, sizeof (T), L"DB04 gotinfo %r attr %lx size %lu",
                                  Status,
                                  (Status == EFI_SUCCESS) ? (UINT64)Info->Attribute : 0,
                                  (Status == EFI_SUCCESS) ? (UINT64)Info->FileSize : 0); Band (T); }
  if (Status != EFI_SUCCESS) { Print (L"DARWINBOOT: getinfo failed\n"); goto Done; }
  Band (L"DB05 check");
  if ((Info->Attribute & EFI_FILE_DIRECTORY) || (Info->FileSize == 0) || (Info->FileSize > Limit)) {
    Print (L"DARWINBOOT: %s attr %lx size %lu limit %lu\n", Path, (UINT64)Info->Attribute, (UINT64)Info->FileSize, (UINT64)Limit);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Done;
  }
  *Size = (UINTN)Info->FileSize;
  //
  // Name and size together, and read as what they are: FileName is the path the
  // file-system driver reports for the request that was made, not proof of the directory
  // entry the medium actually holds. A name that differs from the request is a signal
  // worth looking at; a name that matches does not verify the file.
  //
  { CHAR16 T[128]; UnicodeSPrint (T, sizeof (T), L"DB06 sized %lu phys %lu as %s",
                                  (UINT64)Info->FileSize, (UINT64)Info->PhysicalSize, Info->FileName); Band (T); }

  //
  // Pages, not the pool.
  //
  // A fifty-four megabyte pool allocation makes the pool allocator walk its largest-free
  // descriptor list, and on this board that walk does not finish - the boot stops with
  // "reading kernel" on the screen and nothing after it. AllocatePages asks the page allocator
  // for one contiguous range instead, which is a single lookup.
  //
  //
  // A buffer the kernel can actually be read into.
  //
  // AllocatePages handed back an address far past everything this platform describes, and the
  // read that followed never returned, which is what writing to an address no MMU entry covers
  // looks like here. So the address is checked before it is used, against DRAM_TOP.
  //
  // DRAM_TOP marks the low window this loader allocates from; it is a policy choice made
  // here, not a measurement of all RAM on the device. The firmware's memory map stays the
  // authority on what is actually memory.
  //
  Band (L"DB07 alloc");
  *Data = NULL;
  {
    EFI_PHYSICAL_ADDRESS  Pages;
    EFI_STATUS            AllocateStatus;

    //
    // A conservative selected-low-window cap, kept as it was: the ceiling handed to the
    // allocator is the top of the window minus the buffer. Under the specification
    // AllocateMaxAddress bounds the whole allocation, so DRAM_TOP alone would already be
    // enough; asking lower as well costs nothing and keeps the policy explicit.
    //
    // The range the firmware actually returns is then checked on its own (below), and a
    // returned range that fails that check is freed before *Data is cleared. None of this is
    // a diagnosis of how much RAM the device has.
    //
    // *Size is below 128 MiB for every caller and DRAM_TOP is 3 GiB, so the subtraction does
    // not borrow; it is nevertheless written so that it cannot go below zero.
    //
    Pages = (*Size <= DRAM_TOP) ? (DRAM_TOP - *Size) : 0;
    AllocateStatus = gBS->AllocatePages (AllocateMaxAddress, EfiLoaderData, EFI_SIZE_TO_PAGES (*Size), &Pages);
    if (!EFI_ERROR (AllocateStatus)) {
      //
      // Recorded so that a range that is freed below is only ever freed when the firmware
      // reported one.
      //
      *Data = (VOID *)(UINTN)Pages;
      AllocatedSize = *Size;
      PagesAllocated = TRUE;
    }

    //
    // Subtraction, not addition: an end computed as start + size can wrap, and a wrapped end
    // compares as inside the window.
    //
    if (PagesAllocated &&
        (((UINTN)*Data > (UINTN)DRAM_TOP) || ((UINTN)*Size > (UINTN)(DRAM_TOP - (UINTN)*Data)))) {
      CHAR16  Where[96];

      UnicodeSPrint (Where, sizeof (Where), L"DB07a no page under DRAM top %p", *Data);
      Band (Where);
      //
      // A range the firmware did report is handed back, not dropped: the caller sees
      // *Data NULL and must not free it a second time.
      //
      FreePages (*Data, EFI_SIZE_TO_PAGES (AllocatedSize));
      *Data = NULL;
      PagesAllocated = FALSE;
    }
  }

  {
    CHAR16  Where[96];

    UnicodeSPrint (Where, sizeof (Where), L"DB08 allocated %p size %lu", *Data, (UINT64)*Size);
    Band (Where);
  }
  if (*Data == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Done; }

  //
  // Read in pieces, reporting after each.
  //
  // The whole image in one call was the next place the boot went quiet: the size is not known
  // until GetInfo answers, and a single fifty-four megabyte read leaves nothing to say when it
  // is the read itself that stalls.
  //
  Actual = 0;
  while (Actual < *Size) {
    UINTN   Chunk = *Size - Actual;
    CHAR16  Progress[96];

    if (Chunk > SIZE_8MB) {
      Chunk = SIZE_8MB;
    }

    //
    // One band per piece, before and after the read.
    //
    // Print() writes to a console this board does not paint, so a read that never returns
    // looked exactly like one that had not started. Band() replaces the frame, which is what
    // the rest of this file uses for the same reason.
    //
    UnicodeSPrint (Progress, sizeof (Progress), L"DB09 read %lu of %lu", (UINT64)Actual, (UINT64)*Size);
    Band (Progress);
    Status = File->Read (File, &Chunk, (UINT8 *)*Data + Actual);
    UnicodeSPrint (Progress, sizeof (Progress), L"DB10 read %lu of %lu %r", (UINT64)(Actual + Chunk), (UINT64)*Size, Status);
    Band (Progress);
    if (EFI_ERROR (Status)) { goto Done; }
    if (Chunk == 0) { break; }
    Actual += Chunk;
  }

  Status = EFI_SUCCESS;
  if (!EFI_ERROR (Status) && (Actual != *Size)) { Status = EFI_DEVICE_ERROR; }
Done:
  if (Info != NULL) { FreePages (Info, EFI_SIZE_TO_PAGES (SIZE_OF_EFI_FILE_INFO + 512)); }
  if (File != NULL) { File->Close (File); }
  //
  // Freed with the size the firmware was asked for, and only when it reported a range.
  //
  if (EFI_ERROR (Status) && PagesAllocated && (*Data != NULL)) {
    FreePages (*Data, EFI_SIZE_TO_PAGES (AllocatedSize));
    *Data = NULL;
  }
  return Status;
}

STATIC
EFI_STATUS
ParseKernel (CONST UINT8 *Data, UINTN Size, KERNEL *Kernel)
{
  UINTN    Offset;
  UINTN    End;
  UINTN    Index;
  UINTN    Other;
  UINT32   Count;
  UINT32   Command;
  UINT32   Length;
  SEGMENT  *Segment;
  BOOLEAN  HasEntry;
  BOOLEAN  Executable;

  //
  // Cleared before anything else, and before the header is looked at.
  //
  // A refused header or a NULL Data returns with Count 0, and the caller reports
  // Kernel.Count to the screen: clearing it here keeps a refused image from printing
  // whatever happened to be on the stack. A NULL Data is refused so that a caller that
  // passes nothing gets an error instead of a read. An error found after the header is
  // accepted may return a Count that reflects the segments parsed so far.
  //
  if (Kernel == NULL) { return EFI_INVALID_PARAMETER; }
  ZeroMem (Kernel, sizeof (*Kernel));
  if (Data == NULL) { return EFI_INVALID_PARAMETER; }
  if ((Size < 32) || (U32 (Data) != 0xfeedfacf) || (U32 (Data + 4) != 0x100000c) ||
      (U32 (Data + 8) != 0) || (U32 (Data + 12) != 2)) {
    return EFI_UNSUPPORTED;
  }
  if (U32 (Data + 20) > Size - 32) { return EFI_LOAD_ERROR; }
  Count = U32 (Data + 16);
  End = 32 + U32 (Data + 20);
  Offset = 32;
  HasEntry = FALSE;
  for (Index = 0; Index < Count; Index++) {
    if (End - Offset < 8) { return EFI_LOAD_ERROR; }
    Command = U32 (Data + Offset);
    Length = U32 (Data + Offset + 4);
    if ((Length < 8) || (Length & 7) || (Length > End - Offset)) { return EFI_LOAD_ERROR; }
    if (Command == 0x19) {
      if ((Length < 72) || (U32 (Data + Offset + 64) > (Length - 72) / 80) ||
          (Kernel->Count == MAX_SEGMENTS)) { return EFI_LOAD_ERROR; }
      if (U64 (Data + Offset + 32) != 0) {
        Segment = &Kernel->Segments[Kernel->Count++];
        Segment->Address = U64 (Data + Offset + 24);
        Segment->Size = U64 (Data + Offset + 32);
        Segment->FileOffset = U64 (Data + Offset + 40);
        Segment->FileSize = U64 (Data + Offset + 48);
        Segment->Protection = U32 (Data + Offset + 60);
        if ((Segment->Size > WINDOW_SIZE) || (Segment->Address > MAX_UINT64 - Segment->Size) ||
            (Segment->FileSize > Segment->Size) || (Segment->FileOffset > Size) ||
            (Segment->FileSize > Size - Segment->FileOffset)) { return EFI_LOAD_ERROR; }
      }
    } else if (Command == 5) {
      if (HasEntry || (Length != 288) || (U32 (Data + Offset + 8) != 6) ||
          (U32 (Data + Offset + 12) != 68)) { return EFI_LOAD_ERROR; }
      Kernel->Entry = U64 (Data + Offset + 272);
      HasEntry = TRUE;
    }
    Offset += Length;
  }
  if ((Offset != End) || !HasEntry || (Kernel->Count == 0)) { return EFI_LOAD_ERROR; }
  Kernel->Base = MAX_UINT64;
  Executable = FALSE;
  for (Index = 0; Index < Kernel->Count; Index++) {
    Segment = &Kernel->Segments[Index];
    Kernel->Base = MIN (Kernel->Base, Segment->Address);
    Kernel->End = MAX (Kernel->End, Segment->Address + Segment->Size);
    if ((Kernel->Entry >= Segment->Address) && (Kernel->Entry - Segment->Address < Segment->FileSize) &&
        ((Segment->Protection & 4) != 0)) { Executable = TRUE; }
    for (Other = 0; Other < Index; Other++) {
      if ((Segment->Address < Kernel->Segments[Other].Address + Kernel->Segments[Other].Size) &&
          (Kernel->Segments[Other].Address < Segment->Address + Segment->Size)) { return EFI_LOAD_ERROR; }
    }
  }
  if (!Executable || (Kernel->Entry & 3) || (Kernel->Base & EFI_PAGE_MASK) ||
      (Kernel->Base > MAX_UINT64 - WINDOW_SIZE) ||
      (Kernel->End - Kernel->Base > WINDOW_SIZE - SIZE_2MB)) { return EFI_LOAD_ERROR; }
  return EFI_SUCCESS;
}

STATIC
BOOLEAN
AdtNode (CONST UINT8 *Data, UINTN Size, UINTN *Offset, UINTN Depth, UINTN ParentPath, UINTN *RamdiskOffset, UINTN *TrustCacheOffset, UINTN *AddonOffset, UINT64 *WindowSize, BOOLEAN *KeepPacked)
{
  UINT32 Properties;
  UINT32 Children;
  UINT32 Length;
  UINTN  Index;
  UINTN  Path;
  UINTN  Candidate;
  UINTN  TrustCandidate;
  UINTN  AddonCandidate;
  UINTN  MemoryCandidate;
  CONST UINT8 *Name;
  UINTN  NameLength;

  Name = NULL;
  NameLength = 0;
  Candidate = MAX_UINTN;
  TrustCandidate = MAX_UINTN;
  AddonCandidate = MAX_UINTN;
  MemoryCandidate = MAX_UINTN;
  if ((Depth > 64) || (Size - *Offset < 8)) { return FALSE; }
  Properties = U32 (Data + *Offset);
  Children = U32 (Data + *Offset + 4);
  *Offset += 8;
  if (Properties > (Size - *Offset) / 36) { return FALSE; }
  for (Index = 0; Index < Properties; Index++) {
    if (Size - *Offset < 36) { return FALSE; }
    if (ScanMem8 (Data + *Offset, 32, 0) == NULL) { return FALSE; }
    Length = U32 (Data + *Offset + 32);
    *Offset += 36;
    if (Length > Size - *Offset) { return FALSE; }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "name") == 0) {
      if (Name != NULL) { return FALSE; }
      Name = Data + *Offset;
      NameLength = Length;
    }
    if ((AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "RAMDisk") == 0) ||
        (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0)) {
      if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "PackedRAMDisk") == 0) {
        if (*KeepPacked) { return FALSE; }
        *KeepPacked = TRUE;
      }
      if ((Candidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      Candidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "TrustCache") == 0) {
      if ((TrustCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      TrustCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "GpuAddon") == 0) {
      if ((AddonCandidate != MAX_UINTN) || (Length != 16)) { return FALSE; }
      AddonCandidate = *Offset;
    }
    if (AsciiStrCmp ((CONST CHAR8 *)(Data + *Offset - 36), "darwinboot-memory-size") == 0) {
      if ((MemoryCandidate != MAX_UINTN) || (Length != 8)) { return FALSE; }
      MemoryCandidate = *Offset;
    }
    *Offset += Length;
    if (ALIGN_VALUE (*Offset, 4) > Size) { return FALSE; }
    *Offset = ALIGN_VALUE (*Offset, 4);
  }
  Path = 3;
  if (Depth == 0) { Path = 0; }
  if ((Depth == 1) && (ParentPath == 0) && (NameLength == 7) && (CompareMem (Name, "chosen", 7) == 0)) { Path = 1; }
  if ((Depth == 2) && (ParentPath == 1) && (NameLength == 11) && (CompareMem (Name, "memory-map", 11) == 0)) { Path = 2; }
  if ((Path == 2) && (Candidate != MAX_UINTN)) {
    if (*RamdiskOffset != MAX_UINTN) { return FALSE; }
    *RamdiskOffset = Candidate;
  }
  if ((Path == 2) && (TrustCandidate != MAX_UINTN)) {
    if (*TrustCacheOffset != MAX_UINTN) { return FALSE; }
    *TrustCacheOffset = TrustCandidate;
  }
  if ((Path == 2) && (AddonCandidate != MAX_UINTN)) {
    if (*AddonOffset != MAX_UINTN) { return FALSE; }
    *AddonOffset = AddonCandidate;
  }
  if (MemoryCandidate != MAX_UINTN) {
    if ((Path != 1) || (*WindowSize != 0)) { return FALSE; }
    *WindowSize = U64 (Data + MemoryCandidate);
    if (!DarwinWindowSizeValid (*WindowSize)) { return FALSE; }
  }
  if (Children > (Size - *Offset) / 8) { return FALSE; }
  for (Index = 0; Index < Children; Index++) {
    if (!AdtNode (Data, Size, Offset, Depth + 1, Path, RamdiskOffset, TrustCacheOffset, AddonOffset, WindowSize, KeepPacked)) { return FALSE; }
  }
  return TRUE;
}

STATIC
BOOLEAN
ValidTrustCaches (CONST UINT8 *Data, UINTN Size)
{
  UINTN Count;
  UINTN Index;
  UINTN Start;
  UINTN End;
  UINTN Entries;
  UINTN Entry;

  if (Size < 8) { return FALSE; }
  Count = U32 (Data);
  if ((Count == 0) || (Count > 128) || (Count > (Size - 4) / 4)) { return FALSE; }
  for (Index = 0; Index < Count; Index++) {
    Start = U32 (Data + 4 + Index * 4);
    End = (Index + 1 < Count) ? U32 (Data + 8 + Index * 4) : Size;
    if ((Start < 4 + Count * 4) || (Start & 3) || (End > Size) ||
        (End < Start) || (End - Start < 24) || (U32 (Data + Start) != 2)) { return FALSE; }
    Entries = U32 (Data + Start + 20);
    if ((Entries > (End - Start - 24) / 24) || (End - Start != 24 + Entries * 24)) { return FALSE; }
    for (Entry = 0; Entry < Entries; Entry++) {
      CONST UINT8 *Hash = Data + Start + 24 + Entry * 24;
      if ((Hash[20] < 1) || (Hash[20] > 4) ||
          ((Entry != 0) && (CompareMem (Hash - 24, Hash, 20) >= 0))) { return FALSE; }
    }
  }
  return TRUE;
}

STATIC
EFI_STATUS
GetMap (EFI_MEMORY_DESCRIPTOR **Map, UINTN *Capacity, UINTN *Size, UINTN *Key, UINTN *Stride)
{
  EFI_STATUS Status;
  UINT32     Version;
  UINTN      Attempt;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    *Size = *Capacity;
    Status = gBS->GetMemoryMap (Size, *Map, Key, Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) { return Status; }
    if (*Map != NULL) { FreePool (*Map); }
    *Capacity = *Size + 32 * sizeof (EFI_MEMORY_DESCRIPTOR);
    *Map = AllocatePool (*Capacity);
    if (*Map == NULL) { return EFI_OUT_OF_RESOURCES; }
  }
  return EFI_OUT_OF_RESOURCES;
}

// End of the run of EfiConventionalMemory that begins at Start: the UEFI map often lists physically
// adjacent free ranges as separate descriptors, so extend over every descriptor that continues the run.
STATIC
UINT64
RunEnd (EFI_MEMORY_DESCRIPTOR *Map, UINTN Size, UINTN Stride, UINT64 Start, UINT64 End)
{
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  UINTN                 Offset;
  BOOLEAN               Extended;

  do {
    Extended = FALSE;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->PhysicalStart == End) &&
          (Descriptor->NumberOfPages != 0) && (Descriptor->NumberOfPages < (MAX_UINT64 - End) / EFI_PAGE_SIZE)) {
        End += EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages);
        Extended = TRUE;
      }
    }
  } while (Extended);
  (VOID)Start;
  return End;
}

// Large diagnostic text straight into the GOP framebuffer: the UEFI text console font is too small to
// photograph. 5x12 glyphs from DiagFont.h scaled 4x, white on a black box; wraps at the right edge.
#include "DiagFont.h"
STATIC CONST UINT64 mDiagFont[] = GLYPH_FONT;

STATIC
VOID
DiagDraw (CONST CHAR16 *Text, UINTN Row)
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL *Gop;
  UINT64                       Video[6];
  UINTN                        Scale = 4, X = 0, Y, Index, Gx, Gy, Dx, Dy, Cell, Line;
  UINT32                       *Fb;
  UINT64                       Glyph, Data;
  CHAR16                       Ch;

  if (EFI_ERROR (gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop)) ||
      EFI_ERROR (DarwinBootVideoFromGop (Gop, Video))) { return; }
  Cell = 6 * Scale;
  Line = 14 * Scale;
  Y = Video[4] / 4 + Row * Line;
  Fb = (UINT32 *)(UINTN)Video[0];
  for (Index = 0; Text[Index] != 0; Index++) {
    Ch = Text[Index];
    if ((Ch < 32) || (Ch > 126)) { continue; }
    if (X + Cell > Video[3]) { X = 0; Y += Line; }
    if (Y + Line > Video[4]) { return; }
    Glyph = mDiagFont[Ch - 32];
    for (Gy = 0; Gy < 14 * Scale; Gy++) {
      for (Gx = 0; Gx < Cell; Gx++) {
        Fb[(Y + Gy) * (Video[2] / 4) + X + Gx] = 0xFF000000;
      }
    }
    for (Gy = 0; Gy < 12; Gy++) {
      Data = (Gy < 6) ? ((Glyph >> 32) >> (Gy * 5)) : ((Glyph & 0xFFFFFFFFULL) >> ((Gy - 6) * 5));
      for (Gx = 0; Gx < 5; Gx++) {
        if ((Data >> Gx) & 1) {
          for (Dy = 0; Dy < Scale; Dy++) {
            for (Dx = 0; Dx < Scale; Dx++) {
              Fb[(Y + Gy * Scale + Dy) * (Video[2] / 4) + X + Gx * Scale + Dx] = 0xFFFFFFFF;
            }
          }
        }
      }
    }
    X += Cell;
  }
}

STATIC
EFI_STATUS
AllocateWindow (KERNEL *Kernel, UINT64 WindowSize, EFI_PHYSICAL_ADDRESS *Address)
{
  EFI_MEMORY_DESCRIPTOR *Map;
  EFI_MEMORY_DESCRIPTOR *Descriptor;
  EFI_STATUS            Status;
  UINTN                 Capacity;
  UINTN                 Size;
  UINTN                 Key;
  UINTN                 Stride;
  UINTN                 Offset;
  UINT64                End;
  UINT64                Candidate;
  UINT64                BestStart;
  UINT64                BestBytes;
  UINT64                RunBytes;

  *Address = 0;
  Map = NULL;
  Capacity = 0;
  BestStart = 0;
  BestBytes = 0;
  Status = GetMap (&Map, &Capacity, &Size, &Key, &Stride);
  if (EFI_ERROR (Status)) { if (Map != NULL) { FreePool (Map); } return Status; }
  Status = EFI_OUT_OF_RESOURCES;
  for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
    Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
    if ((Descriptor->Type != EfiConventionalMemory) || (Descriptor->NumberOfPages == 0)) { continue; }
    End = RunEnd (Map, Size, Stride, Descriptor->PhysicalStart,
                  Descriptor->PhysicalStart + EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
    RunBytes = End - Descriptor->PhysicalStart;
    if (RunBytes > BestBytes) { BestBytes = RunBytes; BestStart = Descriptor->PhysicalStart; }
    Candidate = ALIGN_VALUE (Descriptor->PhysicalStart, BLOCK_SIZE) + (Kernel->Base & (BLOCK_SIZE - 1));
    if ((Candidate < Descriptor->PhysicalStart) || (End < Candidate) || (End - Candidate < WindowSize)) { continue; }
    Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);
    if (!EFI_ERROR (Status)) { *Address = Candidate; break; }
  }
  if (EFI_ERROR (Status)) {
    CHAR16 Line[96];
    UINTN  Row = 1;
    UnicodeSPrint (Line, sizeof (Line), L"NO WINDOW %lx BIG RUN %lx @ %lx", WindowSize, BestBytes, BestStart);
    DiagDraw (Line, 0);
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Descriptor = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Descriptor->Type == EfiConventionalMemory) && (Descriptor->NumberOfPages >= 0x400) && (Row < 8)) {
        UnicodeSPrint (Line, sizeof (Line), L"FREE %lx + %lx", Descriptor->PhysicalStart, EFI_PAGES_TO_SIZE (Descriptor->NumberOfPages));
        DiagDraw (Line, Row++);
      }
    }

  }
  FreePool (Map);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
Boot (
  IN UNIXNU_LOADER_PROTOCOL    *This,
  IN CONST UNIXNU_LOADER_BOOT_REQUEST *Request
  )
{
  EFI_STATUS                      Status;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL  *Fs;
  EFI_FILE_PROTOCOL               *Root;
  VOID                            *Image;
  VOID                            *Tree;
  VOID                            *TrustCaches;
  VOID                            *Addon;
  UINTN                           ImageSize;
  UINTN                           TreeSize;
  UINTN                           TrustCacheSize;
  UINTN                           AddonSize;
  CHAR16                          *TreePath;
  UINTN                           Length;
  UINTN                           Index;
  UINTN                           Offset;
  KERNEL                          Kernel;
  EFI_PHYSICAL_ADDRESS            Address;
  UINT64                          ArgsAddress;
  UINT64                          TreeAddress;
  UINT64                          Used;
  UINT64                          WindowSize;
  UINT64                          RamdiskAddress;
  UINTN                           RamdiskOffset;
  UINTN                           RamdiskBytes;
  UINTN                           RamdiskExpandedSize;
  BOOLEAN                         RamdiskPacked;
  BOOLEAN                         KeepPacked;
  UINTN                           TrustCacheOffset;
  UINT64                          TrustCacheAddress;
  UINTN                           AddonOffset;
  UINT64                          AddonAddress;
  DARWIN_BOOT_ARGS                *Args;
  EFI_MEMORY_DESCRIPTOR           *Map;
  UINTN                           Capacity;
  UINTN                           MapSize;
  UINTN                           Key;
  UINTN                           Stride;
  UINT32                          Version;
  UINTN                           Attempt;
  CHAR8                           CommandLine[1024];
  UINT64                          Video[6];

  (VOID)This;

  //
  // Leave a trail for the menu.
  //
  // Print() goes to the console this driver shares with UniXNU, and UniXNU repaints the screen
  // as soon as the boot returns, so nothing printed here survives long enough to be read. The
  // slot is memory UniXNU already knows how to show.
  //
#define DBTRACE(Stage)  (*(volatile UINT64 *)(UINTN)0x9FFFE000 = (UINT64)(Stage))

  DBTRACE (1);
  if ((Request == NULL) || (Request->Volume == NULL) || (Request->KernelPath == NULL)) { return EFI_INVALID_PARAMETER; }
  DBTRACE (2);
  if ((Request->KextCount != 0) || (Request->DeviceTree != NULL) || (Request->DeviceTreeSize != 0) ||
      (DarwinCurrentEl () != 4)) {
    DBTRACE (3);
    return EFI_UNSUPPORTED;
  }
  if (!RamdiskLayout (Request->Ramdisk, Request->RamdiskSize, &RamdiskExpandedSize, &RamdiskPacked)) {
    DBTRACE (4);
    return EFI_BAD_BUFFER_SIZE;
  }
  ZeroMem (CommandLine, sizeof (CommandLine));
  if (Request->BootArgs != NULL) {
    for (Index = 0; Request->BootArgs[Index] != 0; Index++) {
      if ((Index >= sizeof (CommandLine) - 1) || (Request->BootArgs[Index] > 0x7f)) {
        Print (L"DARWINBOOT: boot args rejected at %lu\n", (UINT64)Index);
        return EFI_BAD_BUFFER_SIZE;
      }
      CommandLine[Index] = (CHAR8)Request->BootArgs[Index];
    }
  }
  BandInit ();

  Root = NULL;
  Image = NULL;
  Tree = NULL;
  TrustCaches = NULL;
  Addon = NULL;
  AddonSize = 0;
  TreePath = NULL;
  Map = NULL;
  Address = 0;
  Capacity = 0;
  DBTRACE (5);
  Status = gBS->HandleProtocol (Request->Volume, &gEfiSimpleFileSystemProtocolGuid, (VOID **)&Fs);
  if (EFI_ERROR (Status)) { DBTRACE (0x500 | ((UINT64)Status << 24)); return Status; }
  DBTRACE (6);
  Status = Fs->OpenVolume (Fs, &Root);
  if (EFI_ERROR (Status)) { DBTRACE (0x600 | ((UINT64)Status << 24)); return Status; }
  DBTRACE (7);
  Print (L"DARWINBOOT: reading kernel\n");
  Status = ReadFile (Root, Request->KernelPath, SIZE_128MB, &Image, &ImageSize);
  //
  // The size and the first words together: a file of the right size with the wrong header and a
  // file of the wrong size are different faults, and only this pair tells them apart.
  //
  // Each word is read only once the buffer is known to hold it, and through the unaligned
  // reader the rest of this file uses. On a failed read ImageSize is zero and both words
  // come out zero, so nothing here depends on what a failed read left behind.
  {
    CHAR16  T[128];
    UINTN   Available;
    UINT32  Word0;
    UINT32  Word1;

    Available = (Image == NULL) ? 0 : ImageSize;
    Word0 = (Available >= 4) ? U32 (Image) : 0;
    Word1 = (Available >= 8) ? U32 ((CONST UINT8 *)Image + 4) : 0;
    UnicodeSPrint (T, sizeof (T), L"DB11 read %lu bytes %r first %08x %08x", (UINT64)ImageSize, Status, Word0, Word1);
    Band (T);
  }
  if (EFI_ERROR (Status)) {
    DBTRACE (0x700 | ((UINT64)Status << 24));
    Status = EFI_VOLUME_CORRUPTED;
    goto Fail;
  }
  //
  // What the kernel read actually produced: a wrong entry here looks exactly like a wrong
  // file until the size and the first word are compared. Both come from the reading of the
  // buffer done above, so nothing here dereferences Image again.
  //
  *(volatile UINT64 *)(UINTN)0x9FFFE010 = (UINT64)ImageSize;
  *(volatile UINT64 *)(UINTN)0x9FFFE018 = (Image == NULL) ? 0 : (UINT64)U32 (Image);
  Band (L"DB12 parsing");
  Status = ParseKernel (Image, ImageSize, &Kernel);
  { CHAR16 T[96]; UnicodeSPrint (T, sizeof (T), L"DB13 parsed %r segs %lu", Status, (UINT64)Kernel.Count); Band (T); }
  if (EFI_ERROR (Status)) {
    DBTRACE (0x900 | ((UINT64)Status << 24));
    Status = EFI_LOAD_ERROR;
    goto Fail;
  }
  // Gauguin bring-up checkpoint: leave the successful read/parse result
  // visible before later handoff code can reset or replace the screen.
  // Bounded boot-services delay, not an infinite hold or a kernel change.
  Band (L"DB13 parsed Success: kernel read verified; observing 15 seconds");
  gBS->Stall (15000000);
  Length = StrSize (Request->KernelPath);
  TreePath = AllocatePool (Length + 11 * sizeof (CHAR16));
  if (TreePath == NULL) { Status = EFI_OUT_OF_RESOURCES; goto Fail; }
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".adt");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &Tree, &TreeSize);
  if (EFI_ERROR (Status)) {
    DBTRACE (0x800 | ((UINT64)Status << 24));
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  Offset = 0;
  RamdiskOffset = MAX_UINTN;
  TrustCacheOffset = MAX_UINTN;
  AddonOffset = MAX_UINTN;
  WindowSize = 0;
  KeepPacked = FALSE;
  if (!AdtNode (Tree, TreeSize, &Offset, 0, 3, &RamdiskOffset, &TrustCacheOffset, &AddonOffset, &WindowSize, &KeepPacked) || (Offset != TreeSize)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (KeepPacked && (!RamdiskPacked || (RamdiskOffset == MAX_UINTN))) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  if (WindowSize == 0) { WindowSize = WINDOW_SIZE; }
  DBTRACE (0xA01); Band (L"DB14 A01");
  if (Kernel.Base > MAX_UINT64 - WindowSize) { DBTRACE (0xA02); Print (L"DARWINBOOT: stage A02\n"); Status = EFI_LOAD_ERROR; goto Fail; }
  if (Request->Ramdisk != NULL) {
    if ((RamdiskOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + RamdiskOffset) != 0) ||
        (U64 ((UINT8 *)Tree + RamdiskOffset + 8) != 0)) { Status = EFI_COMPROMISED_DATA; goto Fail; }
  } else if (RamdiskOffset != MAX_UINTN) {
    Status = EFI_UNSUPPORTED;
    goto Fail;
  }
  DBTRACE (0xA03); Print (L"DARWINBOOT: stage A03\n");
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".trustcache");
  Status = ReadFile (Root, TreePath, SIZE_16MB, &TrustCaches, &TrustCacheSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (TrustCaches != NULL) {
    if ((TrustCacheOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + TrustCacheOffset) != 0) ||
        (U64 ((UINT8 *)Tree + TrustCacheOffset + 8) != 0) || !ValidTrustCaches (TrustCaches, TrustCacheSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (TrustCacheOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  //
  // The GPU addon (kernel.macho.gpuaddon, scripts/make-gpu-addon.py): files the kernel writes into the Data volume before its addon
  // mounts. Only its header is checked here; the kernel validates every entry.
  //
  CopyMem (TreePath, Request->KernelPath, Length);
  StrCatS (TreePath, Length / sizeof (CHAR16) + 11, L".gpuaddon");
  Status = ReadFile (Root, TreePath, SIZE_64MB, &Addon, &AddonSize);
  if (EFI_ERROR (Status) && (Status != EFI_NOT_FOUND)) { goto Fail; }
  if (Addon != NULL) {
    if ((AddonOffset == MAX_UINTN) || (U64 ((UINT8 *)Tree + AddonOffset) != 0) || (U64 ((UINT8 *)Tree + AddonOffset + 8) != 0) ||
        (AddonSize < 24) || (CompareMem (Addon, "NGPUADD2", 8) != 0) || (U64 ((UINT8 *)Addon + 16) != AddonSize)) {
      Status = EFI_COMPROMISED_DATA;
      goto Fail;
    }
  } else if (AddonOffset != MAX_UINTN) {
    Status = EFI_COMPROMISED_DATA;
    goto Fail;
  }
  DBTRACE (0xA04); Print (L"DARWINBOOT: stage A04\n");
  Status = ReadBootVideo (Video);
  if (EFI_ERROR (Status)) { DBTRACE (0xA05 | ((UINT64)Status << 24)); goto Fail; }
  DBTRACE (0xA06); Print (L"DARWINBOOT: stage A06\n");
  Status = AllocateWindow (&Kernel, WindowSize, &Address);
  if (EFI_ERROR (Status)) { DBTRACE (0xA07 | ((UINT64)Status << 24)); goto Fail; }
  DBTRACE (0xA08); Print (L"DARWINBOOT: stage A08\n");
  ArgsAddress = ALIGN_VALUE (Address + Kernel.End - Kernel.Base, SIZE_16KB);
  TreeAddress = ArgsAddress + EFI_PAGE_SIZE;
  RamdiskAddress = ALIGN_VALUE (TreeAddress + TreeSize, EFI_PAGE_SIZE);
  RamdiskBytes = ALIGN_VALUE (KeepPacked ? Request->RamdiskSize : RamdiskExpandedSize, EFI_PAGE_SIZE);
  TrustCacheAddress = RamdiskAddress + RamdiskBytes;
  AddonAddress = ALIGN_VALUE (TrustCacheAddress + TrustCacheSize, SIZE_16KB);
  Used = ALIGN_VALUE (AddonAddress + AddonSize, SIZE_16KB) - Address;
  if (Used > WindowSize - BLOCK_SIZE) {
    Print (L"DARWINBOOT: used %lu window %lu kernel %lu ramdisk %lu\n", (UINT64)Used, (UINT64)WindowSize, (UINT64)(Kernel.End - Kernel.Base), (UINT64)RamdiskBytes);
    Status = EFI_BAD_BUFFER_SIZE;
    goto Fail;
  }
  {
    CHAR16  Where[128];

    UnicodeSPrint (Where, sizeof (Where), L"A08a addr %lx used %lx base %lx end %lx",
                   (UINT64)Address, (UINT64)Used, (UINT64)Kernel.Base, (UINT64)Kernel.End);
    Band (Where);
  }

  ZeroMem ((VOID *)(UINTN)Address, (UINTN)Used);
  Band (L"A08b zeroed");
  for (Index = 0; Index < Kernel.Count; Index++) {
    //
    // A segment whose address is below the kernel base would make the destination an
    // underflow - a huge address, and a write to it is what a stop with no output looks like.
    // Bands describe each one before it is copied, so a stop names the segment responsible.
    //
    {
      CHAR16  Where[128];

      UnicodeSPrint (Where, sizeof (Where), L"A08c seg %lu at %lx size %lx file %lx",
                     (UINT64)Index,
                     (UINT64)Kernel.Segments[Index].Address,
                     (UINT64)Kernel.Segments[Index].FileSize,
                     (UINT64)Kernel.Segments[Index].FileOffset);
      Band (Where);
    }

    if (Kernel.Segments[Index].Address < Kernel.Base) {
      Band (L"A08x segment below base");
      Status = EFI_LOAD_ERROR;
      goto Fail;
    }

    CopyMem ((VOID *)(UINTN)(Address + Kernel.Segments[Index].Address - Kernel.Base),
             (UINT8 *)Image + Kernel.Segments[Index].FileOffset, (UINTN)Kernel.Segments[Index].FileSize);
  }
  Band (L"A08d segments copied");
  Args = (DARWIN_BOOT_ARGS *)(UINTN)ArgsAddress;
  Args->Revision = 2;
  Args->Version = 2;
  Args->VirtualBase = Kernel.Base;
  Args->PhysicalBase = Address;
  Args->MemorySize = WindowSize;
  Args->MemorySizeActual = WindowSize;
  Args->TopOfKernelData = Address + Used;
  CopyMem (Args->Video, Video, sizeof (Video));
  Args->DeviceTree = Kernel.Base + TreeAddress - Address;
  Args->DeviceTreeLength = (UINT32)TreeSize;
  CopyMem (Args->CommandLine, CommandLine, sizeof (CommandLine));
  CopyMem ((VOID *)(UINTN)TreeAddress, Tree, TreeSize);
  if (Request->Ramdisk != NULL) {
    if (KeepPacked) { CopyMem ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize); }
    else { CopyRamdisk ((VOID *)(UINTN)RamdiskAddress, Request->Ramdisk, Request->RamdiskSize, RamdiskPacked); }
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset), RamdiskAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + RamdiskOffset + 8), KeepPacked ? Request->RamdiskSize : RamdiskBytes);
  }
  if (TrustCaches != NULL) {
    CopyMem ((VOID *)(UINTN)TrustCacheAddress, TrustCaches, TrustCacheSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset), TrustCacheAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + TrustCacheOffset + 8), TrustCacheSize);
    FreePages (TrustCaches, EFI_SIZE_TO_PAGES (TrustCacheSize)); TrustCaches = NULL;
  }
  if (Addon != NULL) {
    CopyMem ((VOID *)(UINTN)AddonAddress, Addon, AddonSize);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset), AddonAddress);
    WriteUnaligned64 ((UINT64 *)(UINTN)(TreeAddress + AddonOffset + 8), AddonSize);
    FreePages (Addon, EFI_SIZE_TO_PAGES (AddonSize)); Addon = NULL;
  }
  Band (L"A08e freeing image");
  FreePages (Image, EFI_SIZE_TO_PAGES (ImageSize)); Image = NULL;
  Band (L"A08f image freed");
  //
  // Pages out, pages back in.
  //
  // ReadFile hands every buffer back from AllocatePages, and freeing one with FreePool does
  // not come back: the pool keeps its own bookkeeping at the front of what it hands out, and
  // the freed page has none of it. The boot stopped on "A08f image freed" for exactly this.
  //
  FreePages (Tree, EFI_SIZE_TO_PAGES (TreeSize)); Tree = NULL;
  Band (L"A08g tree freed");
  FreePool (TreePath); TreePath = NULL;
  Root->Close (Root); Root = NULL;
  Band (L"A08h root closed");
  DBTRACE (0xA09); Print (L"DARWINBOOT: stage A09\n"); Band (L"A09");
  Status = GetMap (&Map, &Capacity, &MapSize, &Key, &Stride);
  if (EFI_ERROR (Status)) { DBTRACE (0xA0A | ((UINT64)Status << 24)); goto Fail; }
  DBTRACE (0xA0B);
  Print (L"DARWINBOOT: ARM64 va=0x%lx pa=0x%lx args=0x%lx entry=0x%lx owned=0x%lx\n",
         Kernel.Base, Address, ArgsAddress, Address + Kernel.Entry - Kernel.Base, (UINT64)WindowSize);
  Print (L"DARWINBOOT: video base=0x%lx display=%lu row=%lu width=%lu height=%lu depth=%lu\n",
         Video[0], Video[1], Video[2], Video[3], Video[4], Video[5]);
  Print (L"DARWINBOOT: command line: %a\n", CommandLine);
  Print (L"DARWINBOOT: inputs validated; exiting boot services\n");

  //
  // The same facts in large type, in front of the hand-off. The firmware's log draws far
  // larger than the text console, and the form feed ahead of the first line asks the
  // terminal to start at the top; whether it repaints or leaves part of the earlier frame
  // standing is up to the terminal, and PcdDebugPropertyMask (0x07) has the debug print
  // enable bit set, so these lines go out on the standard error device.
  //
  DEBUG ((DEBUG_ERROR, "\fDARWINBOOT: hand-off to the kernel\n\n"));
  DEBUG ((DEBUG_ERROR, "kernel   va 0x%lx  pa 0x%lx\n", Kernel.Base, Address));
  DEBUG ((DEBUG_ERROR, "entry    pa 0x%lx\n", Address + Kernel.Entry - Kernel.Base));
  DEBUG ((DEBUG_ERROR, "args     pa 0x%lx  window 0x%lx bytes\n", ArgsAddress, (UINT64)WindowSize));
  DEBUG ((DEBUG_ERROR, "video    base 0x%lx  %lux%lu  row %lu bytes  %lu bpp\n", Video[0], Video[3], Video[4], Video[2], Video[5]));
  DEBUG ((DEBUG_ERROR, "entry words %08x %08x %08x %08x\n",
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 4)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 8)),
          ReadUnaligned32 ((CONST UINT32 *)(UINTN)(Address + Kernel.Entry - Kernel.Base + 12))));
  DEBUG ((DEBUG_ERROR, "CurrentEL %lu  SCTLR_EL1 0x%lx  TCR_EL1 0x%lx\n", DarwinCurrentEl () >> 2, READ_SYSREG (sctlr_el1), READ_SYSREG (tcr_el1)));
  DEBUG ((DEBUG_ERROR, "ID_AA64MMFR0_EL1 0x%lx  (PARange %lu, 4 KiB granule %a)\n", READ_SYSREG (id_aa64mmfr0_el1),
          READ_SYSREG (id_aa64mmfr0_el1) & 0xF, (((READ_SYSREG (id_aa64mmfr0_el1) >> 28) & 0xF) == 0xF) ? "NOT supported" : "supported"));
  DEBUG ((DEBUG_ERROR, "ID_AA64PFR0_EL1  0x%lx\n", READ_SYSREG (id_aa64pfr0_el1)));
  DEBUG ((DEBUG_ERROR, "\nExiting boot services. A green bar across the top\nmarks the loader handoff before the kernel branch.\n"));
  for (Attempt = 0; Attempt < 3; Attempt++) {
    MapSize = Capacity;
    Status = gBS->GetMemoryMap (&MapSize, Map, &Key, &Stride, &Version);
    if (EFI_ERROR (Status)) {
      if (Attempt == 0) { goto Fail; }
      CpuDeadLoop ();
    }
    Status = gBS->ExitBootServices (mImage, Key);
    if (!EFI_ERROR (Status)) {
      DarwinEnter (ArgsAddress, Address + Kernel.Entry - Kernel.Base, Address, WindowSize, Video[0], Video[2] * Video[4]);
      CpuDeadLoop ();
    }
    if (Status != EFI_INVALID_PARAMETER) { CpuDeadLoop (); }
  }
  CpuDeadLoop ();
  return EFI_ABORTED;
Fail:
  if (Root != NULL) { Root->Close (Root); }
  if (Image != NULL) { FreePages (Image, EFI_SIZE_TO_PAGES (ImageSize)); }
  if (Tree != NULL) { FreePages (Tree, EFI_SIZE_TO_PAGES (TreeSize)); }
  if (TrustCaches != NULL) { FreePages (TrustCaches, EFI_SIZE_TO_PAGES (TrustCacheSize)); }
  if (Addon != NULL) { FreePages (Addon, EFI_SIZE_TO_PAGES (AddonSize)); }
  if (TreePath != NULL) { FreePool (TreePath); }
  if (Map != NULL) { FreePool (Map); }
  if (Address != 0) { gBS->FreePages (Address, EFI_SIZE_TO_PAGES (WindowSize)); }
  Print (L"DARWINBOOT: boot refused: %r; firmware retained\n", Status);
  return Status;
}

STATIC UNIXNU_LOADER_PROTOCOL mLoader = { UNIXNU_LOADER_REVISION, Boot };

STATIC
EFI_STATUS
EFIAPI
Unload (EFI_HANDLE ImageHandle)
{
  return gBS->UninstallMultipleProtocolInterfaces (ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
}

EFI_STATUS
EFIAPI
DarwinBootEntry (EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_LOADED_IMAGE_PROTOCOL *Loaded;
  EFI_STATUS               Status;
  VOID                     *Existing;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol (&gUniXNULoaderProtocolGuid, NULL, &Existing);
  if (!EFI_ERROR (Status)) { return EFI_ALREADY_STARTED; }
  Status = gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded);
  if (EFI_ERROR (Status)) { return Status; }
  mImage = ImageHandle;
  Status = gBS->InstallMultipleProtocolInterfaces (&ImageHandle, &gUniXNULoaderProtocolGuid, &mLoader, NULL);
  if (!EFI_ERROR (Status)) {
    Loaded->Unload = Unload;
    Print (L"DARWINBOOT: loader v2 installed EL%u\n", (UINT32)(DarwinCurrentEl () >> 2));
  }
  return Status;
}
