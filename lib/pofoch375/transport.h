/* CH375 bus transport - selected at link time (today: PID card,
 * tr_pid.c). Ports: 807Dh command, 807Eh data. */

extern unsigned char tr_present();
extern void tr_cmd();
extern void tr_wdata();
extern unsigned char tr_rdata();
extern unsigned char tr_token();
extern void tr_wdata_far();
