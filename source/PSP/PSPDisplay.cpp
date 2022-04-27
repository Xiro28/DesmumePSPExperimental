#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspvfpu.h>
#include <stdio.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psprtc.h>
#include <psppower.h>

#include <string.h>
#include <malloc.h>

#include "../common.h"

#include "../utils/decrypt/header.h"

#include "vram.h"
#include "pspvfpu.h"
#include "PSPDisplay.h"
#include "pspDmac.h"
#include "../GPU.h"
#include "intraFont.h"

#include "pspdisplay.h"
#include "../rasterize.h"

#include <png.h>

#define SLICE_SIZE 16

#define MAX_COL 3
#define ICON_H 150
#define ICON_W 120

#define RGB(r,v,b)	((r) | ((v)<<8) | ((b)<<16) | (0xff<<24))

#define GU_VRAM_WIDTH  512
#define VRAM_START     0x4000000

unsigned int __attribute__((aligned(64))) gulist[256 * 192 * 2];

void*       frameBuffer   =  (void*)0;
const void* doubleBuffer  =  (void*)0x44000;
const void* depthBuffer   =  (void*)0x110000;

const int padding_top    = (1024 * 48);
void*     DISP_POINTER   = (void*)VRAM_START + padding_top;

intraFont* Font, *RomFont;

struct DispVertex {
	unsigned short u, v;
	signed short x, y, z;
};

static void blit_sliced(int sx, int sy, int sw, int sh, int dx, int dy /*, int SLICE_SIZE*/) {
	int start, end;
	// blit maximizing the use of the texture-cache
	for (start = sx, end = sx + sw; start < end; start += SLICE_SIZE, dx += SLICE_SIZE) {
		struct DispVertex* vertices = (struct DispVertex*)sceGuGetMemory(2 * sizeof(struct DispVertex));
		int width = (start + SLICE_SIZE) < end ? SLICE_SIZE : end - start;

		vertices[0].u = start;
		vertices[0].v = sy;
		vertices[0].x = dx;
		vertices[0].y = dy;
		vertices[0].z = 0;

		vertices[1].u = start + width;
		vertices[1].v = sy + sh;
		vertices[1].x = dx + width;
		vertices[1].y = dy + sh;
		vertices[1].z = 0;

		sceGuDrawArray(GU_SPRITES, TEXTURE_FLAGS, 2, NULL, vertices);
	}
}

class Icon {

public:

	u16* GetIconData() {
		return data;
	}

	char* GetIconName() {
		return RomName;
	}

	char* GetDevName() {
		return Developer;
	}

	char* GetFileName() {
		return Filename;
	}

	void SetIconPixel(u8 X, u8 Y, u16 pixel) {
		data[X + (Y * ICON_W)] = pixel;
	}

	void SetIconName(const char* Name) {
		
		if (*Name == '.')
			strcpy(RomName, "Homebrew");
		else
			strcpy(RomName, Name);
		
		RomName[11] = 0;
	}
	void SetDevName(const char* Name) {
		strcpy(Developer, Name);
		Developer[63] = 0;
	}
	void SetFileName(const char* Name) {
		strcpy(Filename, Name);
		Filename[127] = 0;
	}

	void ClearIcon(u16 color) {
		memset(data, color, ICON_W * ICON_H);
	}

	void MEMSetIcon(u16* buff) {
		sceKernelDcacheWritebackInvalidateAll();
		sceDmacMemcpy(data, buff, ICON_W * ICON_H * 2);
	}

private:
	char RomName[12];
	char Developer[64];
	char Filename[128];
	__attribute__((aligned(16))) u16 data[ICON_H * ICON_W * 3];
};

u16 _sel[0/*20*17*2*/];
Icon menu [0/*MAX_COL*/];

void DrawIcon(u16 x, u16 y, u8 sprX, bool curON) {

	sceGuColor(0xffffffff);

	struct DispVertex* vertices = (struct DispVertex*)sceGuGetMemory(2 * sizeof(struct DispVertex));

	const u32 szH = (curON ? ICON_H : (ICON_H/2) + 30);
	const u32 szW = (curON ? ICON_W : (ICON_W/2) + 30);

	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, ICON_W*2, ICON_H, ICON_W, menu[sprX].GetIconData());
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP,GU_CLAMP);

	vertices[0].u = 0;
	vertices[0].v = 0;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = ICON_W;
	vertices[1].v = ICON_H;
	vertices[1].x = x + szW;
	vertices[1].y = y + szH;
	vertices[1].z = 0;

	sceKernelDcacheWritebackInvalidateAll();
	sceGuDrawArray(GU_SPRITES, TEXTURE_FLAGS, 2, NULL, vertices);

}

void DrawSel(u16 x, u16 y, u8 sprX, bool slide) {

	sceGuColor(0xffffffff);

	struct DispVertex* vertices = (struct DispVertex*)sceGuGetMemory(2 * sizeof(struct DispVertex));

	const u32 szH = 17;
	const u32 szW = 20;

	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, szW*2, szH, szW, _sel);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP,GU_CLAMP);

	vertices[0].u = 0;
	vertices[0].v = 0;
	vertices[0].x = x;
	vertices[0].y = y;
	vertices[0].z = 0;

	vertices[1].u = szW;
	vertices[1].v = szH;
	vertices[1].x = x + szW;
	vertices[1].y = y + szH;
	vertices[1].z = 0;

	sceKernelDcacheWritebackInvalidateAll();
	sceGuDrawArray(GU_SPRITES, TEXTURE_FLAGS, 2, NULL, vertices);

}

int curr_posX  = -1;
int curr_page  =  0;
int old_page   = -1;
int N_Roms     =  0;
 
void Set_POSX(int pos) {
	curr_posX = pos;
}

void Set_PAGE(int pos) {
	curr_page = pos;
}

void drawmenu() {

	static u8 last_Xpos = 0;

	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

	sceGuStart(GU_DIRECT, gulist);
	
	sceGuClearColor(0x10404047);
	sceGuClear(GU_COLOR_BUFFER_BIT);

	char buffBattery[16];
	sprintf(buffBattery, "Battery:%d%%", scePowerGetBatteryLifePercent());
	intraFontPrint(Font, 230, 15, "Pre Release");
	intraFontPrint(Font, 410, 15, buffBattery);
	
	for (int x = 30, romX = 0; x < 470; x += 150, romX++) {
		if (N_Roms <= romX) break;
		if (curr_posX == romX) DrawSel(x + 50, 40, romX,(last_Xpos != curr_posX%3));
		//last_Xpos = curr_posX%3;
		DrawIcon(x, 70, romX, true);
		intraFontPrint(Font, x + ((ICON_W+5)/2), 250, menu[romX].GetIconName());
	}


	sceGuFinish();
}

void SetupDisp_EMU() {
	sceGuStart(GU_DIRECT, gulist);
	sceGuDrawBuffer(GU_PSM_5551, (void*)DISP_POINTER, 512);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void EMU_SCREEN() {
	static const int sz_SCR = 256 * 192 * 4;
	sceDmacMemcpy(DISP_POINTER, (const void*)&GPU_Screen, sz_SCR);
}


void Init_PSP_DISPLAY_FRAMEBUFF() {
	static bool inited = false;

	sceGuInit();

	sceGuStart(GU_DIRECT, gulist);

	ScePspFMatrix4 _default = {
		{ 1, 0, 0, 0},
		{ 0, 1, 0, 0},
		{ 0, 0, 1, 0},
		{ 0, 0, 0, 1}
	};

	sceGuDrawBuffer(GU_PSM_5551, frameBuffer, GU_VRAM_WIDTH);
	sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void*)frameBuffer, GU_VRAM_WIDTH);

	// Background color and disable scissor test
	// because it is enabled by default with no size sets
	sceGuClearColor(0xFF404040);
	sceGuDisable(GU_SCISSOR_TEST);

	// Enable clamped rgba texture mode
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexMode(GU_PSM_5551, 0, 1, 0);
	sceGuEnable(GU_TEXTURE_2D);

	// Enable modulate blend mode 
	sceGuEnable(GU_BLEND);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

	sceGuSetMatrix(GU_PROJECTION, &_default);
	sceGuSetMatrix(GU_TEXTURE, &_default);
	sceGuSetMatrix(GU_MODEL, &_default);
	sceGuSetMatrix(GU_VIEW, &_default);

	//sceGuViewport(0, 0, 480, 272);

	// Turn the display on, and finish the current list
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

	return;

	sceGuDisplay(GU_TRUE);

	if (inited) return;
	inited = true;

	static const char* font = "flash0:/font/ltn1.pgf"; //small font
	static const char* font2 = "flash0:/font/ltn0.pgf"; //small font

	intraFontInit();
	Font = intraFontLoad(font, INTRAFONT_CACHE_MED);
	intraFontActivate(Font);
	intraFontSetStyle(Font, 0.6f, 0xFFFFFFFF, 0, 0, INTRAFONT_ALIGN_CENTER);

}

//From: https://github.com/CTurt/IconExtractor/blob/master/source/main.c

Header header;

int readBanner(char* filename, tNDSBanner* banner) {
	FILE* romF = fopen(filename, "rb");
	if (!romF) return 1;

	fread(&header, sizeof(header), 1, romF);
	fseek(romF, header.banner_offset, SEEK_SET);
	fread(banner, sizeof(*banner), 1, romF);
	fclose(romF);

	return 0;
}

void getImageData(u16 * buffer, const char * filename){

	png_structp png_ptr;
	png_infop info_ptr;
	png_uint_32 width, height;
	int bit_depth, color_type, interlace_type;
	FILE *fp;

	char fullpath[256];

	const char * path = "icons/";
	const char * default_icon = "icons/unknown.png";

	strcpy(fullpath,path);
	strncat(fullpath,filename,strlen(filename) - 3);
	strcat(fullpath,"png");
	printf("Path: %s",fullpath);
	

	if ((fp = fopen(fullpath, "rb")) == NULL){
		if ((fp = fopen(default_icon, "rb")) == NULL) return;
	}

	png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (png_ptr == NULL) {
		fclose(fp);
		return;
	}

	info_ptr = png_create_info_struct(png_ptr);
	if (info_ptr == NULL) {
		fclose(fp);
		png_destroy_read_struct(&png_ptr, 0, 0);
		return;
	}

	png_init_io(png_ptr, fp);
	png_set_sig_bytes(png_ptr, 0);
	png_read_info(png_ptr, info_ptr);
	png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, 0, 0);
	png_set_strip_16(png_ptr);
	png_set_packing(png_ptr);

	u16 * line = (u16*) malloc(width * 4);

	for (u16 y = 0; y < height; y++) {
		png_read_row(png_ptr, (u8*) line, 0);
		for (u16 x = 0; x < width * 2; x++) {
			u32 color32 = line[x];
			u16 color16;
			int r = color32 & 0xff;
			int g = (color32 >> 8) & 0xff;
			int b = (color32 >> 16) & 0xff;

			color16 = (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10);
			buffer[x + y * width * 2] = line[x];
		}
	}
	free(line);
	png_read_end(png_ptr, info_ptr);
	png_destroy_read_struct(&png_ptr, &info_ptr, 0);
	fclose(fp);

}


bool CreateRomIcon(char* file, f_list* list) {

	N_Roms = 0;

	tNDSBanner banner;

	for (int c = 0; c < MAX_COL;c++) {

		if (list->cnt <= c) break;

		char rompath[256];

		int index = c + (curr_page * 3);

		if (list->cnt < index) break;

		strcpy(rompath, file);
		strcat(rompath, list->fname[index].name);

		if (readBanner(rompath, &banner)) {
			return false;
		}

		getImageData(menu[c].GetIconData(),list->fname[index].name);

		menu[c].SetIconName(header.title);
		//menu[c].SetDevName(getDeveloperNameByID(atoi(header.makercode)).c_str());
		//menu[c].SetFileName(list->fname[index].name);
		N_Roms++;
	}

return true;
}

void DrawRom(char* file, f_list* list, int pos, bool reload) {

	char rompath[256];
	char RomFileName[128];
	//Get rom file path 
	strcpy(rompath, file);

	curr_page = pos / 3;
	curr_posX = pos % 3;	

	if (old_page != curr_page) {
		CreateRomIcon(rompath, list);
		getImageData(_sel,"sel.   ");
		old_page = curr_page;
	}
	drawmenu();
}