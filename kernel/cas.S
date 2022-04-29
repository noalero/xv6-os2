.global cas

    cas:
        lr.w t0, (a0)                       # Load original value from address in a0
        bne t0, a1, fail                    # Expected value in a1 doesn't match the loaded value in t1, so fail
        sc.w a0, a2, (a0)                   # Try to update- to put the desired value from a2 to the address in a0
        jr ra                               # Return

    fail:
        li a0, 1                            # Set return to failure- a0 is the returned value, 1 is !0
        jr ra                               # Return