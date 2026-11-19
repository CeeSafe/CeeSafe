# ADR 0178 — Printing a PDF through the system's print dialog

- Status: **Accepted** by the owner, 2026-11-19 (asked what IPos's Print should do for the A4 invoice and the voucher:
  "System print dialog"). macOS, iOS and Android implemented the same day (§5); not released. Amendment 1 (§6,
  Windows) accepted by the owner the same day.
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
  dialog: an item with a path is printed, one without a cancel (`tools/print_check.py`).
- **Per system.** macOS: PDFKit's `PDFDocument` and its print operation (scaled to the paper, turned to fit), loaded
  with dlopen. iOS: `UIPrintInteractionController` (AirPrint). Android: `PrintManager` with a document adapter of
  the runtime's dex that gives it the file. Windows: the print dialog, and Windows' own PDF renderer (§6). Linux
  and FreeBSD: `Unsupported` (deferred, as ADR 0177).

## 3. Options not taken

- **Printing the pages as images** through each system's page API: the PDF already is the document, with its text
  as text, and every system prints a PDF. (Except Windows, whose desktop API has no print of a PDF:
  §6.)
- **`lpr`, or the default PDF viewer's print verb**: no dialog, no choice of printer, and not on phones.

## 4. Order of work

1. macOS (done, §5). 2. iOS (done, §5). 3. Android (done, §5). 4. Windows (§6). Until a system's step, `printFile` is
`Unsupported` there.

## 5. Implementation (2026-11-19)

macOS: `cee_native_print` (ceesafe_rt_window.c). IPos's preview has Print… (every document), beside Save PDF.

iOS: `cee_native_print` (ceesafe_rt_window.c), `UIPrintInteractionController` given the file's URL (`canPrintURL:`
first: `Resources` when it is not a PDF it reads), presented as a sheet; the run loop runs until its completion handler,
a block without captures, says printed, cancelled or failed (an error is `Resources`). `Unsupported` when the device
has no printing (`isPrintingAvailable`).

Android: `cee_native_print` (ceesafe_rt_window.c) starts and waits for the dialog as a picker is (ADR 0158), with the
runtime's `Printing.java` in the embedded dex: a file that does not begin as a PDF is `Resources` at once; else
`PrintManager.print` with a document adapter that copies the file. When the dialog is gone (`onFinish`) the job is
still "created" for a while (a quarter of a second on Android 16): its state is read every tenth of a second until
it is set, for 30 seconds at most. Queued, started, blocked or completed (the dialog's own "Save as PDF") is printed;
cancelled, or still created, is not; failed is `Resources`.

## 6. Amendment 1: Windows (2026-11-19)

Accepted by the owner (asked how Windows should print, with the print dialog and Windows' PDF renderer, the default
PDF app's print verb, or later: "Dialog + Windows' PDF renderer").

**Context.** Win32 has no call that gives a PDF to a print dialog: its print dialog gives a printer's device context,
and what is drawn on it is printed. Windows' print UI for a document (`Windows.Graphics.Printing`) wants pages drawn
by the app as well, through XAML or DirectX. The default PDF app's print verb is §3's, and answers nothing.

**Decision.** On Windows, `printFile`:

1. loads the file with `Windows.Data.Pdf.PdfDocument` (WinRT, in every Windows 10 and 11; reached from C through its
   COM interfaces, combase.dll and shcore.dll loaded at first use): a file it does not read is `Resources` before any
   dialog;
2. shows the common print dialog (`PrintDlgExW`, owned by the window): the printer, its preferences, copies and the
   pages (all, or ranges of the document's page count). Cancel is `false`;
3. prints each chosen page on the dialog's device context: the page rendered by `PdfPage.RenderToStreamAsync` to a
   bitmap at the printer's resolution (at most 300 dots per inch, which a page of text and lines does not need
   more of), scaled to fit the printable area with its proportions kept and centred, turned when the page and the
   paper differ in orientation, as macOS's print operation does. `true` when the document was given to the spooler;
   a page that fails ends the job (`AbortDoc`) and is `Resources`.

The pages are bitmaps (§3's first option), on Windows only: Windows has no way to print a PDF's own text and lines
that every machine has.

**Checking.** Printing to a printer needs a person; `tools/print_check.py` (Windows CI) checks steps 1 and 3 with
`CEESAFE_PRINT_PRINTER` naming a printer and `CEESAFE_PRINT_OUTPUT` a file: the dialog is skipped, and the document is
printed to that printer with that output file (Microsoft Print to PDF writes a PDF there). The two variables are for
checking only, as ADR 0177's script is.
