# ADR 0271 — Contacts: the address book, and the system's contact picker

- Status: **Accepted** by the owner 2026-11-27, with §6 answered (reading every contact and a picker; a name and phone
  numbers; the phones and macOS), for Marivil's CeeUI port, whose MAUI app reads the address book to show who is on
  Marivil. Implemented the same day (§7).
- Scope: `lib/std/window.cee` (`Contact`, `Contacts`), `runtime/hosted/ceesafe_rt_window.c` (dialog kinds 11 and 12),
  `runtime/android/org/ceesafe/window/AddressBook.java` (new) and `Picker.java`, type-system.md §26.13,
  `spec/api/std.md`, `tools/window_check.py`. The permission's declarations are the packer's (CeeUI's `ceeui-pack
  --contacts`: Android's `READ_CONTACTS`, iOS's and macOS's `NSContactsUsageDescription`).

## 1. Context

- MAUI's new-chat page reads the address book (after its own consent prompt: the numbers go to the server to be
  matched), and shows the people already on Marivil, a tap opening a chat, and the others, a tap inviting them
  through the share sheet. MAUI uses `Contacts.Default.GetAllAsync()`, a display name and phone numbers.
- iOS and macOS: Contacts.framework's `CNContactStore` (`requestAccessForEntityType:completionHandler:`,
  `enumerateContactsWithFetchRequest:error:usingBlock:`), needing the app's `NSContactsUsageDescription` (iOS ends an
  app that asks without it); ContactsUI's `CNContactPickerViewController` gives one contact with no permission (the
  Mac's `CNContactPicker` is a popover beside a view, not a modal picker).
- Android: `ContactsContract`'s phone numbers through the content resolver, with `READ_CONTACTS` asked at run time;
  `ACTION_PICK` on the phone numbers grants the one row chosen, with no permission.
- Windows, Linux and FreeBSD have no common address book.

## 2. Decision

```
let everyone = Contacts.read(mut window)?;      // Vector<Contact>: name, phones
let one = Contacts.pick(mut window)?;           // Contact?, null at a cancel
```

- **`Contacts.read(window)`** asks the system's permission once, then gives every contact: its name (given and family,
  else the organization) and its phone numbers as written. `Resources` when the permission is refused or the app does
  not declare it (on iOS and macOS, no `NSContactsUsageDescription`).
- **`Contacts.pick(window)`** is the system's picker, which needs no permission: iOS gives the contact with its
  numbers; Android's picks one phone number of a contact, so the contact has that one number. Null at a cancel.
- Windows, Linux, FreeBSD, and `pick` on the Mac: `Unsupported`. A scripted window takes its next `dialog` answer:
  `name:number:number|name:number`, each contact an answer.
- The answer from the runtime is one string per contact: the name, then each number after a unit separator (0x1F).

## 3. Consequences

- Marivil's port can show who of the address book is on Marivil (after its own consent, as MAUI asks), and pick a
  contact to write to.
- An app that reads contacts must declare why (the packer's `--contacts`).

## 4. Options not taken

- **Emails and photos** (§6.2): Marivil matches phone numbers; less is read and held.
- **A picker on the Mac**: ContactsUI's popover needs a view to anchor to and a delegate; it can come when a Mac app
  needs it.
- **Windows' contacts** (WinRT's `ContactStore`) and Linux's (Evolution Data Server): no common, simple API.

## 5. Tests

- `tools/window_check.py`: a scripted window's address book (three contacts, one with two numbers and one with none)
  and a picked contact.
- The Moto and the owner's iPhone, in Marivil's port: the permission asked, the address book read, a contact picked.

## 6. Questions for the owner (answered 2026-11-27)

1. **Scope**: reading every contact as MAUI does (recommended), that and a picker, or a picker only? *Both.*
2. **Fields**: a name and phone numbers (recommended), or emails and photos too? *A name and phone numbers.*
3. **Systems**: the phones and macOS (recommended), or the phones only? *The phones and macOS.*

## 7. Implementation (2026-11-27)

- `lib/std/window.cee`: `Contact` and `Contacts.read` and `pick` over `FileDialog.run` (kinds 11 and 12), splitting
  each answer at 0x1F.
- iOS: `CNContactStore` with two blocks without captures (the access's answer, which the run loop waits for, and each
  contact, added as an answer); the picker through the dialogs' `CeeSafePickerDelegate`, which now answers
  `contactPicker:didSelectContact:` and `contactPickerDidCancel:`. The usage description is looked for first.
- macOS: the same store and blocks (the access's answer awaited with a short sleep: it comes on another queue).
- Android: `AddressBook.java` (in the dex) reads the phone numbers grouped by contact; `Picker` asks `READ_CONTACTS`
  first and starts `ACTION_PICK` on the phone numbers for `pick`.
- The window runtime compiles with `-Wall -Wextra -Wcast-qual -Werror` for macOS, iOS, Android, Linux (gcc 14, clang
  19) and Windows (MinGW-w64 gcc 16).
