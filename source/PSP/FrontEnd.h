#ifndef FRONTEND
#define FRONTEND
//const char *nds_file;

class configured_features 
{
public:

	bool enable_sound;
	bool showfps;
	bool swap;
	bool cur;
	bool Render3D;
	bool fps_cap;
	bool FastMERendering;
	bool extrmarm7down;

	bool gpuLayerEnabled[2][5];

	int frameskip;
	int hide_screen;
	int fps_cap_num;
	int DynarecBlockSize;
	int VcountStart;
	int firmware_language;
	int savetype;
};

typedef struct configparm {
	char name[32];
	int var;
}configP;

typedef struct fname {
	char name[256];
}f_name;

typedef struct flist {
	f_name fname[256];
	f_name dir[256];
	int cnt;
	int dir_cnt;
}f_list;

extern configured_features my_config;

extern void ChangeRom(bool reset);
extern void ResetRom();
extern void EMU_Conf();
void InitDisplayParams(configured_features* params);
void EXEC_NDS();

void DrawTouchPointer();

void DoConfig(configured_features * params);

void DSEmuGui(char *path,char *out);

void WriteLog(char* msg);

#endif
