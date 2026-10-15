/*
 * tpmute: send a devd event when the ThinkPad EC speaker mute changes.
 *
 * The firmware mutes the speaker itself and raises no ACPI notify, so a
 * kernel process polls dev.acpi_ibm.0.mute and calls devctl_notify() on a
 * change:  !system=ACPI subsystem=IBM type=MUTE state=0|1
 *
 * The mute is read through the acpi_ibm sysctl, not its softc, so the module
 * does not depend on acpi_ibm's private layout.  acpi_ibm has no
 * MODULE_VERSION, so there is no MODULE_DEPEND on it either: the module
 * refuses to load without the sysctl (except at boot), and keeps the last
 * state while it is gone (acpi_ibm unloaded).
 *
 * On the ThinkPad A475 it also sets the speaker mute LED from the EC's mute
 * (hw.tpmute.led).  The firmware sets that LED only from its own copy of the
 * mute (AUDC, through the SMI behind SSMS/GSMS), which lags the EC's; there
 * the LED is AMD FCH GPIO 143 (DSDT: \_SB.PCI0.SMB.G143, bit 22 of the
 * register at 0xFED81500 + 0x23C), low lights it.  The module writes only
 * that bit, only on that model, and only if the pin is an output.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/devctl.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#define	TPMUTE_SYSCTL		"dev.acpi_ibm.0.mute"
#define	TPMUTE_INTERVAL_MIN	50
#define	TPMUTE_INTERVAL_MAX	10000

#define	TPMUTE_LED_MODEL	"ThinkPad A475"	/* smbios.system.version */
#define	TPMUTE_LED_REG		0xFED8173CUL
#define	TPMUTE_LED_OUT		(1u << 22)	/* output value, low = lit */
#define	TPMUTE_LED_OE		(1u << 23)	/* output enable */

/*
 * MTX_SYSINIT: the sysctl handlers lock it, and the linker removes the
 * sysctls only after MOD_UNLOAD, so it must outlive tpmute_unload().
 */
static struct mtx	tpmute_mtx;
MTX_SYSINIT(tpmute, &tpmute_mtx, "tpmute", MTX_DEF);
static struct proc	*tpmute_proc;
static bool		tpmute_stop;
static int		tpmute_interval_ms = 200;
static int		tpmute_state = -1;
static int		tpmute_oid[CTL_MAXNAME];
static u_int		tpmute_oidlen;
static void		*tpmute_led_map;	/* NULL: not this model */
static volatile uint32_t *tpmute_led_reg;
static int		tpmute_led = 1;

static int
tpmute_sysctl_interval(SYSCTL_HANDLER_ARGS)
{
	int error, val;

	val = tpmute_interval_ms;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (val < TPMUTE_INTERVAL_MIN || val > TPMUTE_INTERVAL_MAX)
		return (EINVAL);
	/* Wake the thread so that the new interval applies now. */
	mtx_lock(&tpmute_mtx);
	tpmute_interval_ms = val;
	wakeup(&tpmute_stop);
	mtx_unlock(&tpmute_mtx);
	return (0);
}

static int
tpmute_sysctl_led(SYSCTL_HANDLER_ARGS)
{
	int error, val;

	val = (tpmute_led_reg == NULL) ? -1 : tpmute_led;
	error = sysctl_handle_int(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (tpmute_led_reg == NULL || (val != 0 && val != 1))
		return (EINVAL);
	tpmute_led = val;
	return (0);
}

static SYSCTL_NODE(_hw, OID_AUTO, tpmute, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "ThinkPad EC mute to devd");
SYSCTL_PROC(_hw_tpmute, OID_AUTO, interval_ms,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    tpmute_sysctl_interval, "I", "Poll interval in milliseconds");
SYSCTL_INT(_hw_tpmute, OID_AUTO, state, CTLFLAG_RD, &tpmute_state, 0,
    "Last mute state seen (-1: unknown)");
SYSCTL_PROC(_hw_tpmute, OID_AUTO, led,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    tpmute_sysctl_led, "I",
    "Set the speaker mute LED from the EC (-1: not on this model)");

/* Set the LED to the mute, if it is not already; the firmware may set it too. */
static void
tpmute_led_set(int mute)
{
	uint32_t v, want;

	if (tpmute_led_reg == NULL || tpmute_led != 1)
		return;
	v = *tpmute_led_reg;
	if ((v & TPMUTE_LED_OE) == 0)
		return;
	want = mute ? (v & ~TPMUTE_LED_OUT) : (v | TPMUTE_LED_OUT);
	if (want != v)
		*tpmute_led_reg = want;
}

/* Map the LED's GPIO register, on the A475 only. */
static void
tpmute_led_attach(void)
{
	char *model;
	bool match;

	model = kern_getenv("smbios.system.version");
	match = (model != NULL && strcmp(model, TPMUTE_LED_MODEL) == 0);
	if (model != NULL)
		freeenv(model);
	if (!match)
		return;
	tpmute_led_map = pmap_mapdev(trunc_page(TPMUTE_LED_REG), PAGE_SIZE);
	tpmute_led_reg = (volatile uint32_t *)((char *)tpmute_led_map +
	    (TPMUTE_LED_REG & PAGE_MASK));
	if ((*tpmute_led_reg & TPMUTE_LED_OE) == 0) {
		printf("tpmute: mute LED pin is not an output, not using it\n");
		pmap_unmapdev(tpmute_led_map, PAGE_SIZE);
		tpmute_led_map = NULL;
		tpmute_led_reg = NULL;
	}
}

/* Look up the OID of TPMUTE_SYSCTL; it changes if acpi_ibm is reloaded. */
static int
tpmute_lookup(void)
{
	int qoid[2] = { CTL_SYSCTL, CTL_SYSCTL_NAME2OID };
	size_t len, rlen;
	int error;

	len = sizeof(tpmute_oid);
	error = kernel_sysctl(curthread, qoid, 2, tpmute_oid, &len,
	    __DECONST(char *, TPMUTE_SYSCTL), strlen(TPMUTE_SYSCTL), &rlen, 0);
	tpmute_oidlen = (error == 0) ? rlen / sizeof(int) : 0;
	return (error);
}

static int
tpmute_read(int *val)
{
	size_t len;
	int error;

	if (tpmute_oidlen == 0 && (error = tpmute_lookup()) != 0)
		return (error);
	len = sizeof(*val);
	error = kernel_sysctl(curthread, tpmute_oid, tpmute_oidlen, val, &len,
	    NULL, 0, NULL, 0);
	if (error != 0)
		tpmute_oidlen = 0;
	return (error);
}

static void
tpmute_thread(void *arg __unused)
{
	int val;

	mtx_lock(&tpmute_mtx);
	while (!tpmute_stop) {
		mtx_unlock(&tpmute_mtx);
		if (tpmute_read(&val) == 0) {
			val = (val != 0);
			if (tpmute_state != -1 && val != tpmute_state)
				devctl_notify("ACPI", "IBM", "MUTE",
				    val ? "state=1" : "state=0");
			tpmute_state = val;
			tpmute_led_set(val);
		}
		mtx_lock(&tpmute_mtx);
		if (tpmute_stop)
			break;
		msleep(&tpmute_stop, &tpmute_mtx, 0, "tpmute",
		    MAX(1, tpmute_interval_ms * hz / 1000));
	}
	mtx_unlock(&tpmute_mtx);
	/* Wakes up tpmute_unload(), which sleeps on tpmute_proc. */
	kproc_exit(0);
}

static void
tpmute_led_detach(void)
{
	if (tpmute_led_map != NULL)
		pmap_unmapdev(tpmute_led_map, PAGE_SIZE);
	tpmute_led_map = NULL;
	tpmute_led_reg = NULL;
}

static int
tpmute_load(void)
{
	int error, val;

	/* From loader.conf acpi_ibm has not attached yet: start anyway. */
	if ((error = tpmute_read(&val)) == 0)
		tpmute_state = (val != 0);
	else if (!cold) {
		printf("tpmute: cannot read %s (error %d); acpi_ibm loaded?\n",
		    TPMUTE_SYSCTL, error);
		return (ENXIO);
	}
	TUNABLE_INT_FETCH("hw.tpmute.led", &tpmute_led);
	tpmute_led_attach();
	tpmute_stop = false;
	error = kproc_create(tpmute_thread, NULL, &tpmute_proc, 0, 0,
	    "tpmute");
	if (error != 0)
		tpmute_led_detach();
	return (error);
}

static void
tpmute_unload(void)
{
	/* Also called after a failed MOD_LOAD, with no thread. */
	if (tpmute_proc == NULL)
		return;
	mtx_lock(&tpmute_mtx);
	tpmute_stop = true;
	wakeup(&tpmute_stop);
	msleep(tpmute_proc, &tpmute_mtx, PWAIT, "tpmexit", 0);
	tpmute_proc = NULL;
	mtx_unlock(&tpmute_mtx);
	tpmute_led_detach();
}

static int
tpmute_modevent(module_t mod __unused, int type, void *data __unused)
{
	switch (type) {
	case MOD_LOAD:
		return (tpmute_load());
	case MOD_UNLOAD:
		tpmute_unload();
		return (0);
	default:
		return (EOPNOTSUPP);
	}
}

static moduledata_t tpmute_mod = {
	"tpmute",
	tpmute_modevent,
	NULL
};

DECLARE_MODULE(tpmute, tpmute_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(tpmute, 1);
