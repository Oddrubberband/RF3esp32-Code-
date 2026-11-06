Calibration source: voltage_sensing_unconnected.csv, user supplied 5 November 2026.
Original supplied-file SHA-256: 9b79c66d36c086808ee664832e081633ff19b4425b970b5cf7fb38446dd802ad.

Column A: bench pack voltage in volts. Column B: unconnected sense voltage in
volts. Ignore the other columns (including the isolated ratio annotation).
There are 41 valid ordered pairs. Row38 has24.25V but no sense measurement and
is omitted, not replaced by zero. Preserve2.399999V as2399999microvolts.

For neighboring table points (s0,p0) and(s1,p1), reconstruct pack millivolts:
p = p0 + (s-s0)*(p1-p0)/(s1-s0), where s is mean sense microvolts.
Use the first/last segment for extrapolation and floor negative pack values at0.
Keep microvolt precision through conversion; round sense millivolts for display
only afterward. The table is monotonic and spans15.25–25.50V pack.

Calculated from the supplied points:
20.400V sensed corresponds to2.225600V sense input.
21.000V recovery corresponds to2.291100V sense input.
The omitted24.25V row lies halfway between24.0V/2.618V and24.5V/2.672V,
so linear interpolation gives2.645V; this is calculated, not a measured point.
Display correction: max(0, sensed_pack_mv-100). Example23.040V ->22.940V.
The existing generic6S curve gives46%,1518mAh nominal at that estimate.

These are software mapping checks, not proof of loaded circuit or ADC accuracy.
Exact input hashes and independent check evidence accompany the verification.

Git normalizes line endings to LF. Measurement text is unchanged.
The repository CSV SHA-256 is 517eb6f2469d04b482540610aa7d4eaf3ea092434961c97ea6dcaa2c2a766036.
See source_record.json for both identities and units.
