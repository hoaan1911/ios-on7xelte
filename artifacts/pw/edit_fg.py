P = '/workspace/chats/c57811c4ae693748619586f4/artifacts/pw/fg.new.c'
L = open(P).read().splitlines(True)

def find(sub, start=0):
    for i in range(start, len(L)):
        if sub in L[i]:
            return i
    raise SystemExit('not found: ' + sub)

# the slave address becomes a variable the scan can move
i = find('#define FG_ADDR')
L[i:i + 1] = ['static uint8_t fg_addr = 0x76u;     /* 7-bit 0x3b; the scan moves it if the gauge is elsewhere */\n']
i = find('fg_wr(FG_DS, FG_ADDR | (receive ? 1u : 0u));')
L[i:i + 1] = ['\tfg_wr(FG_DS, (uint32_t)fg_addr | (receive ? 1u : 0u));\n']

INIT = '''int
a5xelte_fg_init(void)
{
	int enabled = 1;

	(void)PE_parse_boot_argn("j7fg", &enabled, sizeof(enabled));
	if (!enabled) {
		return -1;
	}
	fg_clock_on();
	/* Samsung's tree puts the gauge on I2C4 at 0x3b, and this phone answers on none of the
	 * six controllers at that address.  The charger/FG part is an S2MU005 block whose sibling
	 * PMIC answers as 0x66, so ask the three addresses it can have, on the bus Samsung
	 * documents first and then on the rest.  The first pair that ACKs is kept. */
	static const uint32_t buses[] = {
		0x13830000u, 0x13840000u, 0x13850000u, 0x13860000u, 0x13870000u,
		0x13880000u, 0x13890000u,
	};
	static const uint8_t addrs[] = { 0x76u, 0x66u, 0x1bu };

	for (unsigned i = 0; i < sizeof(buses) / sizeof(buses[0]); i++) {
		fg_i2c = ml_io_map(buses[i], 0x1000);
		if (!fg_i2c) {
			continue;
		}
		for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
			uint16_t soc = 0xffff;
			fg_addr = addrs[a];
			fg_errors = 1000000u;   /* the scan logs itself, one line per bus */
			if (fg_word(FG_REPSOC, &soc)) {
				fg_errors = 0;
				printf("A5XELTE FG: gauge on 0x%08x at 0x%02x, RSOC=%#x\\n",
				    buses[i], fg_addr, soc);
				return 0;
			}
		}
		printf("A5XELTE FG: nothing on 0x%08x, stat=%#x con=%#x\\n",
		    buses[i], fg_rd(FG_STAT), fg_rd(FG_CON));
	}
	printf("A5XELTE FG: no gauge answered on any I2C bus\\n");
	return -1;
}

'''

READ = '''int
a5xelte_fg_read(unsigned *percent, unsigned *mv, int *ma, unsigned *rem_mah,
    unsigned *full_mah)
{
	uint16_t soc, vbat, cur, cap;
	unsigned raw, p;

	if (!fg_i2c || !fg_word(FG_REPSOC, &soc) || !fg_word(FG_RVBAT, &vbat) ||
	    !fg_word(FG_RCUR, &cur) || !fg_word(FG_RBATCAP, &cap)) {
		return -1;
	}
	/* Two layouts are in the vendor tree for this register: hundredths of a percent
	 * (0..10000, full = capacity_max 99.0 %) and 1/16384 of full.  Take the one the value
	 * fits instead of assuming, which is why the percentage never matched the phone. */
	raw = soc & 0x7fffu;
	if (raw <= 10000u) {
		p = raw * 1000u / FG_CAPACITY_MAX;
	} else {
		p = raw * 1000u / 16384u;
	}
	*percent = p >= 1000u ? 100u : p / 10u;
	*mv = (unsigned)(((uint64_t)(vbat & 0x7fffu) * 1000u) >> 13);
	*ma = (cur & 0x8000u)
	    ? -(int)(((((unsigned)~cur & 0xffffu) + 1u) * 1000u) >> 12)
	    :  (int)((((unsigned)cur & 0x7fffu) * 1000u) >> 12);
	/* RBATCAP is in the gauge's own units; left at zero rather than invented. */
	*rem_mah = 0;
	*full_mah = 0;
	(void)cap;
	return 0;
}
'''

i = find('a5xelte_fg_init(void)', 150)
j = find('a5xelte_fg_read(unsigned *percent', 150)
assert L[i - 1].rstrip('\n') == 'int' and L[j - 1].rstrip('\n') == 'int'
L = L[:i - 1] + INIT.splitlines(True) + L[j - 1:]

i = find('a5xelte_fg_read(unsigned *percent', 150)
k = max(n for n, line in enumerate(L) if line.rstrip('\n') == '#endif')
L = L[:i - 1] + READ.splitlines(True) + L[k:]
open(P, 'w').write(''.join(L))
print('ok')
