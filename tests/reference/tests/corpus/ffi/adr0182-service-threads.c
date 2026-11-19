/* ADR 0182: adr0182-service-threads.cee needs no C of its own. It is an ffi test because its services outlive main,
   which ends with Process.exit and so prints no allocation report: run-pass programs are fault-swept, and the sweep
   needs that report (ADR 0182 section 4, open). */
typedef int adr0182_no_c_of_its_own;
