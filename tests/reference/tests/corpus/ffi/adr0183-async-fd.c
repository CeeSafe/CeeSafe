/* ADR 0183: adr0183-async-fd.cee calls the C library (pipe, fcntl, read, write, close) and needs no C of its own.
   It is an ffi test so that --interp corpus runs, which run no C, skip it. */
typedef int adr0183_no_c_of_its_own;
