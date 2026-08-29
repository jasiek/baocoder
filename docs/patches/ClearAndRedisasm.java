// Clear stale code units over a range and re-disassemble, then recreate the
// functions.  Needed after fixing a processor module: Ghidra records an error
// at an undecodable instruction and does not retry it, so a corrected sleigh
// has no effect on addresses it has already failed at.
//@category Repair
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class ClearAndRedisasm extends GhidraScript {
    @Override
    public void run() throws Exception {
        long[][] ranges = { {0x0002509cL, 0x0002515fL}, {0x00025160L, 0x00025223L} };
        String[] names  = { "Dsp_FftFinalStageButterfly", "Dsp_FftStageButterfly" };
        for (int i = 0; i < ranges.length; i++) {
            Address start = toAddr(ranges[i][0]);
            Address end   = toAddr(ranges[i][1]);
            Function f = getFunctionAt(start);
            if (f != null) {
                removeFunction(f);
            }
            clearListing(start, end);
            disassemble(start);
            Function fn = createFunction(start, names[i]);
            println(names[i] + ": body " + (fn != null ? fn.getBody().getNumAddresses() : 0) + " bytes");
        }
    }
}
