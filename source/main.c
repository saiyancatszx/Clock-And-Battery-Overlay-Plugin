#include "global.h"
#include "ov.h"

FS_archive sdmcArchive = { 0x9, (FS_path){ PATH_EMPTY, 1, (u8*)"" } };
Handle fsUserHandle = 0;

#define CALLBACK_OVERLAY (101)

#define __datetime_selector        (*(vu32*)0x1FF81000)
#define __datetime0 (*(volatile datetime_t*)0x1FF81020)
#define __datetime1 (*(volatile datetime_t*)0x1FF81040)

#define TICKS_PER_MSEC (268123.480)

typedef struct {
	u64 date_time;
	u64 update_tick;
	//...
} datetime_t;

static Handle ptmuHandle;
static Handle mcuHandle;

Result ptmuInit(void)
{
	Result res = srv_getServiceHandle(NULL, &ptmuHandle, "ptm:u");
	return res;
}

static datetime_t getSysTime(void) {
	u32 s1, s2 = __datetime_selector & 1;
	datetime_t dt;

	do {
		s1 = s2;
		if(!s1)
			dt = __datetime0;
		else
			dt = __datetime1;
		s2 = __datetime_selector & 1;
	} while(s2 != s1);

	return dt;
}

u64 osGetTime(void) {
	datetime_t dt = getSysTime();

	u64 delta = svc_getSystemTick() - dt.update_tick;

	return dt.date_time + (u64)(((double)(delta))/TICKS_PER_MSEC);
}

static inline u32 IPC_MakeHeader(u16 command_id, unsigned normal_params, unsigned translate_params)
{
	return ((u32) command_id << 16) | (((u32) normal_params & 0x3F) << 6) | (((u32) translate_params & 0x3F) << 0);
}

Result mcuInit(void)
{
    return (srv_getServiceHandle(NULL, &mcuHandle, "mcu::HWC"));
}

Result  MCU_GetBatteryLevel(u8* out)
{
    u32* ipc = getThreadCommandBuffer();
    ipc[0] = IPC_MakeHeader(0x5,0,0); // 0x50000
    Result ret = svc_sendSyncRequest(mcuHandle);
    if(ret < 0)
        return ret;
    *out = ipc[2];
    return ipc[1];
}

#define SECONDS_IN_DAY 86400
#define SECONDS_IN_HOUR 3600
#define SECONDS_IN_MINUTE 60
#define LIMEGREEN   128,255,0
#define YELLOW      255,255,0
#define RED         255,0,0
#define BLUE        0,0,255
#define WHITE       255,255,255
#define BLACK         0,0,0

//am pm string
static char * g_speriod[] = { "am", "pm"};
int     TopScreenPx = 400;
int     WidgetPosX = 340;
int     WidgetPosY = 14;

void drawWidget(int batPercent, u32 addr, u32 stride, u32 format, u8 hour, u8 min, u8 seconds, int colOffset) {

	char buf[30]={0};
	
	
	//Draw background
	ovDrawTranspartBlackRect(addr, stride, format, WidgetPosY, WidgetPosX,11,60,1);
	
	
	// Handle AM/PM
	int period = 0;
	if (hour >= 12) period = 1;

	// 12 Hour Clock because 'mesia
	if 		(hour==0) 	hour = 12;
	else if (hour > 12) hour = hour - 12;
	
	//Draw Clock
	if(seconds%2==0){
		if		(period==1) xsprintf(buf, "%02d:%02dpm", hour, min);
		else	xsprintf(buf, "%02d:%02dam", hour, min);
	}
	else{
		if		(period==1) xsprintf(buf, "%02d %02dpm", hour, min);
		else	xsprintf(buf, "%02d %02dam", hour, min);
	}
	
	ovDrawString(addr, stride, format, TopScreenPx, WidgetPosY+1, WidgetPosX+1, WHITE, buf);
	
	//Draw Battery
	ovDrawRect(addr, stride, format, WidgetPosY + 2, WidgetPosX + 46, 1, 10, WHITE); //topline
	ovDrawRect(addr, stride, format, WidgetPosY + 8, WidgetPosX + 46, 1, 10, WHITE); //bottomline
	ovDrawRect(addr, stride, format, WidgetPosY + 2, WidgetPosX + 45, 7, 1, WHITE); //leftline
	ovDrawRect(addr, stride, format, WidgetPosY + 2, WidgetPosX + 56, 7, 1, WHITE); //rightline
	ovDrawRect(addr, stride, format, WidgetPosY + 4, WidgetPosX + 57, 3, 1, WHITE); //battery terminal
	
	int batval = batPercent/10;
	
	if(batPercent >= 80)					ovDrawRect(addr, stride, format, WidgetPosY + 3, WidgetPosX + 46, 5, batval, LIMEGREEN); //battery content
	else if(batPercent >= 30 && batPercent <= 80)	ovDrawRect(addr, stride, format, WidgetPosY + 3, WidgetPosX + 46, 5, batval, YELLOW); //battery content
	else 								ovDrawRect(addr, stride, format, WidgetPosY + 3, WidgetPosX + 46, 5, batval, RED); //battery content
	
	//Draw background
	ovDrawTranspartBlackRect(addr, stride, format, 234, 383,7,17,1);
	
	//Draw Percent
	xsprintf(buf, "%03d%%", batPercent);
	ovDrawStringTiny(addr, stride, format, TopScreenPx, 235, 384, WHITE, buf);
}

/*
Overlay Callback.
isBottom: 1 for bottom screen, 0 for top screen.
addr: writable cached framebuffer virtual address.
addrB: right-eye framebuffer for top screen, undefined for bottom screen.
stride: framebuffer stride(pitch) in bytes, at least 240*bytes_per_pixel.
format: framebuffer format, see https://www.3dbrew.org/wiki/GPU/External_Registers for details.

NTR will invalidate data cache of the framebuffer before calling overlay callbacks. NTR will flush data cache after the callbacks were called and at least one overlay callback returns zero.

return 0 when the framebuffer was modified. return 1 when nothing in the framebuffer was modified.
*/

u32 overlayCallback(u32 isBottom, u32 addr, u32 addrB, u32 stride, u32 format) {
	static u32 count = 0;
	static u8 batPercent = 0;

	u32 height = isBottom ? 320 : 400;
	if (isBottom == 0) {
		if (count == 0) {
			MCU_GetBatteryLevel(&batPercent);
		}
		count ++;
		if (count > 100) {
			count = 0;
		}

		u64 timeInSeconds = osGetTime() / 1000;
		u64 dayTime = timeInSeconds % SECONDS_IN_DAY;
		u8 hour = dayTime / SECONDS_IN_HOUR;
		u8 min = (dayTime % SECONDS_IN_HOUR) / SECONDS_IN_MINUTE;
		u8 seconds = dayTime % SECONDS_IN_MINUTE;
	
		drawWidget(batPercent, addr, stride, format, hour, min, seconds, WidgetPosX);
		// In 2D mode, top screen's addrB might be invalid or equal to addr, do not draw on addrB in either situations
		if ((addrB) && (addrB != addr))  {
			drawWidget(batPercent, addrB, stride, format, hour, min, seconds, WidgetPosX-4);
		}
		return 0;
	}
	return 1;
}

int main() {
	u32 retv;
	
	initSharedFunc();
	// Init srv client and ptmu client for monitoring battery status.
	initSrv();
	ptmuInit();
	mcuInit();
	// Register overlay callback.
	plgRegisterCallback(CALLBACK_OVERLAY, (void*) overlayCallback, 0);
	return 0;
}

