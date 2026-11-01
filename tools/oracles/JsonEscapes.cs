// Prints the BMP code points that System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping (as used by
// the diagnostics' JSON through Utf8JsonWriter) writes escaped, as inclusive ranges "lo hi" in hex.
using System.Text.Encodings.Web;
using System.Text.Json;
var options = new JsonWriterOptions { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };
int? start = null;
for (var c = 0; c <= 0x10000; c++)
{
    var escaped = false;
    if (c < 0x10000 && (c < 0xD800 || c > 0xDFFF))
    {
        using var buffer = new MemoryStream();
        using (var writer = new Utf8JsonWriter(buffer, options))
        {
            writer.WriteStringValue(((char)c).ToString());
        }
        var text = System.Text.Encoding.UTF8.GetString(buffer.ToArray());
        escaped = text.Contains('\\');
    }
    else if (c < 0x10000)
    {
        escaped = true; // lone surrogates: not text, always escaped
    }
    if (escaped && start is null) start = c;
    if (!escaped && start is not null) { Console.WriteLine($"{start:X4} {c - 1:X4}"); start = null; }
}
if (start is not null) Console.WriteLine($"{start:X4} FFFF");
Console.Error.WriteLine(System.Environment.Version);
