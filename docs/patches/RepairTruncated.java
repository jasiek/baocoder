// Re-disassemble functions that were truncated by an undecodable instruction.
//@category Repair
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class RepairTruncated extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] raw = getScriptArgs();
        StringBuilder sb = new StringBuilder();
        for (String r : raw) sb.append(r).append(" ");
        String joined = sb.toString().replace("[","").replace("]","").replace(",", " ").replace("\"","");
        for (String a : joined.trim().split("\\s+")) {
            if (a.isEmpty()) continue;
            Address start = toAddr(Long.parseLong(a, 16));
            Function f = getFunctionAt(start);
            if (f == null) { println(a + ": no function"); continue; }
            String name = f.getName();
            long before = f.getBody().getNumAddresses();
            Address end = start.add(0x800);
            removeFunction(f);
            clearListing(start, end);
            disassemble(start);
            Function fn = createFunction(start, name);
            long after = fn != null ? fn.getBody().getNumAddresses() : 0;
            println(name + " @" + a + ": " + before + " -> " + after + " bytes");
        }
    }
}
