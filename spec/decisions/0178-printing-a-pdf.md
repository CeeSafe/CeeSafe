# ADR 0178 — Printing a PDF through the system's print dialog

- Status: **Accepted** by the owner, 2026-11-19 (asked what IPos's Print should do for the A4 invoice and the voucher:
  "System print dialog"). macOS and iOS implemented the same day (§5); not released.
- Scope: lib/std's `FileDialog.printFile` (type-system.md §26.13), the window runtime's dialogs (a sixth kind), the
  guide. No change to the language.

## 1. Context

IPos makes each document as a PDF (ceesafe_reporting's port of CosmoPdfExport) and can save it; the .NET app printed
the server's PDF through the system. An A4 invoice or a voucher goes to an office or network printer, which only the
system knows: CeeSafe had no way to reach its print dialog. (Receipts go to a Bluetooth thermal printer, ADR 0177.)

## 2. Decision

- **`FileDialog.printFile(window, title, file) -> Result<bool, WindowError>`**: the PDF at `file` given to the system's
  print dialog, `title` the job's name. `true` when it was printed (or saved by the dialog's own PDF button), `false`
  when the user cancelled. Modal, as the file dialogs are (ADR 0146). `Resources` when the file is not a PDF the
  system reads.
- **The runtime** runs it as a sixth kind of the window's dialog (`CEE_DIALOG_PRINT`), the path where an export's
  file is: one empty path answers "printed", none "cancelled". A scripted window answers it from its script as any
  dialog.
- **Per system.** macOS: PDFKit's `PDFDocument` and its print operation (scaled to the paper, turned to fit), loaded
  with dlopen. iOS: `UIPrintInteractionController` (AirPrint). Android: `PrintManager` with a document adapter of
  the runtime's dex that gives it the file. Windows: the system's print dialog for a PDF. Linux and FreeBSD:
  `Unsupported` (deferred, as ADR 0177).

## 3. Options not taken

- **Printing the pages as images** through each system's page API: the PDF already is the document, with its text
  as text, and every system prints a PDF.
- **`lpr`, or the default PDF viewer's print verb**: no dialog, no choice of printer, and not on phones.

## 4. Order of work

1. macOS (done, §5). 2. iOS (done, §5). 3. Android. 4. Windows. Until a system's step, `printFile` is `Unsupported` there.

## 5. Implementation (2026-11-19)

macOS: `cee_native_print` (ceesafe_rt_window.c). IPos's preview has Print… (every document), beside Save PDF.

iOS: `cee_native_print` (ceesafe_rt_window.c), `UIPrintInteractionController` given the file's URL (`canPrintURL:`
first: `Resources` when it is not a PDF it reads), presented as a sheet; the run loop runs until its completion handler,
a block without captures, says printed, cancelled or failed (an error is `Resources`). `Unsupported` when the device
has no printing (`isPrintingAvailable`).
