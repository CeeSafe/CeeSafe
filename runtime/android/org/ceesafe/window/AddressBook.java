// SPDX-License-Identifier: MIT
// Contacts (ADR 0271): the address book read whole (READ_CONTACTS, asked by Picker first), or one contact's number
// chosen in the system's picker (ACTION_PICK on the phone numbers, which grants that one row and needs no
// permission). Each contact is its name, then its numbers, each after a unit separator (0x1F), and a zero byte after
// it, the answer the runtime gives the program (Picker.answer: 1 with contacts, 0 cancelled, -1 failed).

package org.ceesafe.window;

import android.app.Activity;
import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.ContactsContract;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;

final class AddressBook {
    private static final int FAILED = -1;
    private static final int CANCELLED = 0;
    private static final int DONE = 1;

    private AddressBook() {}

    /** Every contact with a phone number: its display name and its numbers, in the address book's order of names. */
    static void answerAll(Activity activity) {
        LinkedHashMap<Long, String> names = new LinkedHashMap<Long, String>();
        LinkedHashMap<Long, ArrayList<String>> numbers = new LinkedHashMap<Long, ArrayList<String>>();
        ContentResolver resolver = activity.getContentResolver();
        String[] columns = {
            ContactsContract.CommonDataKinds.Phone.CONTACT_ID,
            ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME,
            ContactsContract.CommonDataKinds.Phone.NUMBER,
        };
        try (Cursor rows = resolver.query(ContactsContract.CommonDataKinds.Phone.CONTENT_URI, columns, null, null,
                ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME + " ASC")) {
            if (rows == null) {
                Picker.answer(FAILED, null);
                return;
            }
            while (rows.moveToNext()) {
                long id = rows.getLong(0);
                String name = rows.getString(1);
                String number = rows.getString(2);
                if (number == null || number.isEmpty()) {
                    continue;
                }
                if (!names.containsKey(id)) {
                    names.put(id, name != null ? name : "");
                    numbers.put(id, new ArrayList<String>());
                }
                ArrayList<String> mine = numbers.get(id);
                if (!mine.contains(number)) {
                    mine.add(number);
                }
            }
        } catch (RuntimeException e) {
            Picker.answer(FAILED, null);
            return;
        }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (Long id : names.keySet()) {
            write(out, names.get(id), numbers.get(id));
        }
        Picker.answer(DONE, out.toByteArray());
    }

    /** The phone number chosen in the picker (`uri`, a row of the phone numbers, granted for this answer). */
    static void answerPicked(Activity activity, Uri uri) {
        String[] columns = {
            ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME,
            ContactsContract.CommonDataKinds.Phone.NUMBER,
        };
        try (Cursor row = activity.getContentResolver().query(uri, columns, null, null, null)) {
            if (row == null || !row.moveToFirst()) {
                Picker.answer(CANCELLED, null);
                return;
            }
            ArrayList<String> one = new ArrayList<String>();
            String number = row.getString(1);
            if (number != null && !number.isEmpty()) {
                one.add(number);
            }
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            write(out, row.getString(0), one);
            Picker.answer(DONE, out.toByteArray());
        } catch (RuntimeException e) {
            Picker.answer(FAILED, null);
        }
    }

    /** One contact: its name, each number after 0x1F, then a zero byte (separators in the texts are dropped). */
    private static void write(ByteArrayOutputStream out, String name, ArrayList<String> numbers) {
        byte[] n = clean(name).getBytes(StandardCharsets.UTF_8);
        out.write(n, 0, n.length);
        for (String number : numbers) {
            out.write(0x1F);
            byte[] b = clean(number).getBytes(StandardCharsets.UTF_8);
            out.write(b, 0, b.length);
        }
        out.write(0);
    }

    private static String clean(String text) {
        return text == null ? "" : text.replace('\u001f', ' ').replace('\u0000', ' ');
    }
}
