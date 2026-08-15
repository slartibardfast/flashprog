---- MODULE gfxati_flash ----
(***************************************************************************)
(* The ordering contract of the gfxati flash model: transaction framing,  *)
(* the write-enable discipline, and the AAI continuation rule. The byte   *)
(* semantics (AND-only program, 0xFF erase, per-chip geometry) live in    *)
(* the allium spec and the C self-test; this spec pins the orderings.     *)
(***************************************************************************)
EXTENDS Naturals

CONSTANT A, PAGESZ  \* address space 0..A-1; page size in cells (PAGESZ divides A)

VARIABLES
    phase,     \* "idle", "addr", "data"
    pending,   \* "none", "read", "pp", "se", "ce", "aai", "wrsr"
    wel,       \* write-enable latch
    aai_on,    \* AAI mode active (survives transaction ends until WRDI)
    latch,     \* address being assembled, 0..A-1
    addr,      \* current command address (page number)
    step,      \* position inside the current phase (0-based)
    cells      \* per-cell marker: 0 untouched, 1 programmed

vars == <<phase, pending, wel, aai_on, latch, addr, step, cells>>

PAGES == A \div PAGESZ

Init ==
    /\ phase = "idle"
    /\ pending = "none"
    /\ wel = FALSE
    /\ aai_on = FALSE
    /\ latch = 0
    /\ addr = 0
    /\ step = 0
    /\ cells = [a \in 0..A-1 |-> 0]

(***************************************************************************)
(* Commands. A command byte only takes effect in the idle phase. The      *)
(* write-class commands (pp, se, ce, aai) require the write-enable latch; *)
(* a write command without WEL is ignored. WREN sets the latch; WRDI      *)
(* clears it and ends AAI.                                                *)
(***************************************************************************)
Cmd(c) ==
    /\ phase = "idle"
    /\ pending = "none"
    /\ IF c = "read" THEN
          /\ phase' = "addr" /\ pending' = "read" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "pp" /\ wel THEN
          /\ phase' = "addr" /\ pending' = "pp" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "se" /\ wel THEN
          /\ phase' = "addr" /\ pending' = "se" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "ce" /\ wel THEN
          /\ phase' = "idle" /\ pending' = "none" /\ step' = 0
          /\ wel' = FALSE /\ aai_on' = aai_on
          /\ cells' = [a \in 0..A-1 |-> 0]
       ELSE IF c = "aai" /\ wel /\ aai_on THEN
          /\ phase' = "data" /\ pending' = "aai" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "aai" /\ wel /\ \lnot aai_on THEN
          /\ phase' = "addr" /\ pending' = "aai" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "wrsr" THEN
          /\ phase' = "data" /\ pending' = "wrsr" /\ step' = 0
          /\ wel' = wel /\ aai_on' = aai_on
       ELSE IF c = "wren" THEN
          /\ phase' = "idle" /\ pending' = "none" /\ step' = 0
          /\ wel' = TRUE /\ aai_on' = aai_on
       ELSE IF c = "wrdi" THEN
          /\ phase' = "idle" /\ pending' = "none" /\ step' = 0
          /\ wel' = FALSE /\ aai_on' = FALSE
       ELSE
          /\ UNCHANGED vars
    /\ UNCHANGED <<latch, addr, cells>>

(***************************************************************************)
(* Address bytes. Only the address phase accepts them; a command's        *)
(* address is complete after step = 2 (three bytes, modeled as two bits   *)
(* for the small constant instance).                                      *)
(***************************************************************************)
AddrByte(b) ==
    /\ phase = "addr"
    /\ step < 2
    /\ step' = step + 1
    /\ latch' \in 0..A-1
    /\ UNCHANGED <<phase, pending, wel, aai_on, addr, cells>>

AddrComplete ==
    /\ phase = "addr"
    /\ step = 2
    /\ pending \in {"read", "pp", "se", "aai"}
    /\ addr' = latch \div PAGESZ
    /\ step' = 0
    /\ phase' = "data"
    /\ aai_on' = IF pending = "aai" THEN TRUE ELSE aai_on
    /\ UNCHANGED <<pending, wel, latch, cells>>

(***************************************************************************)
(* The data phase. A program byte lands only while the command is pp or   *)
(* aai; the read command streams without writes.                          *)
(***************************************************************************)
DataByte(d) ==
    /\ phase = "data"
    /\ pending = "pp"
    /\ step < PAGESZ
    /\ cells' = [cells EXCEPT ![addr * PAGESZ + step] = 1]
    /\ step' = step + 1
    /\ UNCHANGED <<phase, pending, wel, aai_on, latch, addr>>

DataByteAai(d) ==
    /\ phase = "data"
    /\ pending = "aai"
    /\ cells' = [cells EXCEPT ![addr * PAGESZ + step] = 1]
    /\ step' = IF step = 1 THEN 0 ELSE 1
    /\ addr' = IF step = 1 THEN (addr + 1) \div PAGES ELSE addr
    /\ UNCHANGED <<phase, pending, wel, aai_on, latch>>

ReadByte ==
    /\ phase = "data"
    /\ pending = "read"
    /\ step < A
    /\ step' = step + 1
    /\ UNCHANGED <<phase, pending, wel, aai_on, latch, addr, cells>>

(***************************************************************************)
(* Transaction end: the host raises CS. A completed write-class command   *)
(* clears WEL and, for an erase, restores the page (or the whole chip).   *)
(* AAI stays armed across transaction ends until WRDI.                    *)
(***************************************************************************)
CsEnd ==
    /\ phase \in {"data", "addr"}
    /\ phase' = "idle"
    /\ pending' = "none"
    /\ step' = 0
    /\ wel' = IF pending \in {"pp", "se", "ce"} THEN FALSE ELSE wel
    /\ cells' =
        IF pending = "se" THEN
            [a \in 0..A-1 |->
                IF addr * PAGESZ <= a /\ a < (addr + 1) * PAGESZ THEN 0 ELSE cells[a]]
        ELSE IF pending = "ce" THEN
            [a \in 0..A-1 |-> 0]
        ELSE
            cells
    /\ UNCHANGED <<aai_on, latch, addr>>

Next ==
    \/ Cmd("read") \/ Cmd("pp") \/ Cmd("se") \/ Cmd("ce")
    \/ Cmd("aai") \/ Cmd("wrsr") \/ Cmd("wren") \/ Cmd("wrdi")
    \/ \E b \in 0..1: AddrByte(b)
    \/ AddrComplete
    \/ \E d \in 0..1: DataByte(d)
    \/ \E d \in 0..1: DataByteAai(d)
    \/ ReadByte
    \/ CsEnd

(***************************************************************************)
(* Safety.                                                                *)
(***************************************************************************)
TypeOK ==
    /\ phase \in {"idle", "addr", "data"}
    /\ pending \in {"none", "read", "pp", "se", "ce", "aai", "wrsr"}
    /\ wel \in BOOLEAN
    /\ aai_on \in BOOLEAN
    /\ latch \in 0..A-1
    /\ addr \in 0..PAGES
    /\ step \in 0..A
    /\ cells \in [0..A-1 -> 0..1]

\* A write in flight implies the latch was set when the command was accepted:
\* the data phase of a write-class command only runs under WEL.
InvWrData ==
    phase = "data" /\ pending \in {"pp", "aai", "se"} => wel

\* The AAI mode is armed once the first word's address is latched, so a
\* data phase under pending = "aai" always runs with the mode on.
InvAaiArmed ==
    phase = "data" /\ pending = "aai" => aai_on

\* The address latch never leaves the address space.
InvAddrBound ==
    phase = "addr" => latch \in 0..A-1

Spec == Init /\ [][Next]_vars

THEOREM Spec => []TypeOK
====
