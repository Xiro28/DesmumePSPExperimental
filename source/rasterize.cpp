/*
	Copyright (C) 2009-2015 DeSmuME team
	This file is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 2 of the License, or
	(at your option) any later version.
	This file is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	You should have received a copy of the GNU General Public License
	along with the this software.  If not, see <http://www.gnu.org/licenses/>.
*/

//nothing in this file should be assumed to be accurate
//
//the shape rasterizers contained herein are based on code supplied by Chris Hecker from 
//http://chrishecker.com/Miscellaneous_Technical_Articles


//TODO - due to a late change of a y-coord flipping, our winding order is wrong
//this causes us to have to flip the verts for every front-facing poly.
//a performance improvement would be to change the winding order logic
//so that this is done less frequently

#include "rasterize.h"

#include <algorithm>
#include <assert.h>
#include <math.h>
#include <string.h>

#if defined(_MSC_VER) && _MSC_VER == 1600
#define SLEEP_HACK_2011
#endif

#ifdef SLEEP_HACK_2011
#include <Windows.h>
#endif

#ifndef _MSC_VER 
#include <stdint.h>
#endif

#include "bits.h"
#include "common.h"
#include "matrix.h"
#include "render3D.h"
#include "gfx3d.h"
#include "texcache.h"
#include "MMU.h"
#include "GPU.h"
#include "NDSSystem.h"

#include "PSP/pspvfpu.h"
#include "PSP/vram.h"
#include "PSP/PSPDisplay.h"
#include "PSP/pspvfpu.h"
#include "PSP/pspDmac.h"

#include <pspkernel.h>
#include <pspdisplay.h>

#include <pspgu.h>
#include <pspgum.h>
#include <malloc.h>


#define __MEM_START 0x04000000


inline void* vrelptr(void* ptr)
{
	return (void*)((u32)ptr & ~__MEM_START);
}

volatile u32 _screen[GFX3D_FRAMEBUFFER_WIDTH * GFX3D_FRAMEBUFFER_HEIGHT];


CACHE_ALIGN const float divide5bitBy31_LUT[32] = { 0.0,             0.0322580645161, 0.0645161290323, 0.0967741935484,
													   0.1290322580645, 0.1612903225806, 0.1935483870968, 0.2258064516129,
													   0.2580645161290, 0.2903225806452, 0.3225806451613, 0.3548387096774,
													   0.3870967741935, 0.4193548387097, 0.4516129032258, 0.4838709677419,
													   0.5161290322581, 0.5483870967742, 0.5806451612903, 0.6129032258065,
													   0.6451612903226, 0.6774193548387, 0.7096774193548, 0.7419354838710,
													   0.7741935483871, 0.8064516129032, 0.8387096774194, 0.8709677419355,
													   0.9032258064516, 0.9354838709677, 0.9677419354839, 1.0 };

static bool softRastHasNewData = false;


struct PolyAttr
{ 
	u32 val;

	bool decalMode;
	bool translucentDepthWrite;
	bool drawBackPlaneIntersectingPolys;
	u8 polyid;
	u8 alpha;
	bool backfacing;
	bool translucent;
	u8 fogged;

	bool isVisible(bool backfacing) 
	{
		//this was added after adding multi-bit stencil buffer
		//it seems that we also need to prevent drawing back faces of shadow polys for rendering
		u32 mode = (val>>4)&0x3;
		if(mode==3 && polyid !=0) return !backfacing;
		//another reasonable possibility is that we should be forcing back faces to draw (mariokart doesnt use them)
		//and then only using a single bit buffer (but a cursory test of this doesnt actually work)
		//
		//this code needs to be here for shadows in wizard of oz to work.

		switch((val>>6)&3) {
			case 0: return false;
			case 1: return backfacing;
			case 2: return !backfacing;
			case 3: return true;
			default: /*assert(false);*/ return false;
		}
	}

	void setup(u32 polyAttr)
	{
		val = polyAttr;
		decalMode = BIT14(val);
		translucentDepthWrite = BIT11(val);
		polyid = (polyAttr>>24)&0x3F;
		alpha = (polyAttr>>16)&0x1F;
		drawBackPlaneIntersectingPolys = BIT12(val);
		fogged = BIT15(val);
	}

};

template<bool RENDERER>
class RasterizerUnit
{
public:
	RasterizerUnit(){}

	int polynum;

    PolyAttr polyAttr;
	SoftRasterizerEngine* engine;

	struct Vertex* __attribute__((aligned(32))) vertices;

	union{
		struct{u8 a; u8 b ; u8 g; u8 r;};
		u32 color;
	}ArraytoColor;

	void SetupViewport(const u32 viewportValue) {
		sceGuViewport(0, 192,512,384);
	}

	u32 roundToExp2(u32 val)
	{
		u32 ret = 1;
		while(ret < val) ret <<= 1;
		return ret;
	}

	void SetupTexture(POLY& thePoly) {
		
		if (thePoly.texParam == 0 || thePoly.getTexParams().texFormat == TEXMODE_NONE) {
			sceGuDisable(GU_TEXTURE_2D);
		}
		else {

			TexCacheItem* newTexture = TexCache_SetTexture(TexFormat_32bpp, thePoly.texParam, thePoly.texPalette);

			sceGuEnable(GU_TEXTURE_2D);
			sceGumMatrixMode(GU_TEXTURE);

			/*sceGuTexFlush();
			sceGuTexProjMapMode(GU_UV);*/

			sceGuTexMode(GU_PSM_8888, 0, 0, 0);
			//sceGuTexFilter(GU_NEAREST, GU_NEAREST);
			sceGuTexWrap(BIT16(newTexture->texformat) ? GU_REPEAT : GU_CLAMP, BIT17(newTexture->texformat) ? GU_REPEAT : GU_CLAMP);

			u16 __attribute__((aligned(16))) tbw = newTexture->bufferWidth;
			sceGuTexImage(0, roundToExp2(newTexture->sizeX), roundToExp2(newTexture->sizeY), tbw, newTexture->decoded);
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
			//sceGuTexScale(newTexture->invSizeX, newTexture->invSizeY);
		}
	}

	void SetupPoly(POLY& thePoly)
	{
		const PolygonAttributes attr = thePoly.getAttributes();

		static const short GUDepthFunc[2] = { GU_LESS, GU_EQUAL };

		//sceGuDepthFunc(GUDepthFunc[attr.enableDepthTest]);

		bool enableDepthWrite = true;

		 if (attr.isTranslucent)
		{
			//sceGuDisable(GU_STENCIL_TEST);
			//glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			enableDepthWrite = (!attr.isTranslucent || ((attr.polygonMode == POLYGON_MODE_DECAL) && attr.isOpaque) || attr.enableAlphaDepthWrite) ? true : false;
		}
		else
		{
			sceGuEnable(GU_STENCIL_TEST);
			sceGuStencilFunc(GU_ALWAYS, 0x80, 0xFF);
			sceGuStencilOp(GU_KEEP, GU_KEEP, GU_REPLACE);
			//glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			enableDepthWrite = true;
		}

		if (!attr.isOpaque)
		{
			sceGuEnable(GU_ALPHA_TEST);
			sceGuAlphaFunc(GU_GREATER,0,0xFF);
		}

	//	sceGuDepthMask(enableDepthWrite);
	}

	FORCEINLINE void mainLoop()
	{

		using std::min;
		using std::max;

		const size_t polyCount = engine->clippedPolyCounter;

		if (polyCount == 0) {
			memset((u32*)&_screen[0], 0,  192 * 256 * 4);
			return;
		}

		const void* renderTarget __attribute__((aligned(16))) = (void*)0x44000;

		static const int GUPrimitiveType[] = { GU_TRIANGLES , GU_TRIANGLE_FAN , GU_TRIANGLES , GU_TRIANGLE_FAN , GU_LINE_STRIP, GU_LINE_STRIP,GU_LINE_STRIP,GU_LINE_STRIP };

		static const int sz[] = { 3, 4, 3, 4, 3, 4, 3, 4 };

		bool first = true;
		int VertListIndex = 0;

		const ScePspFMatrix4 _matrx __attribute__((aligned(16))) = {
			{0.998f, 0, 0, 0},
			{ 0, 0.998f, 0, 0},
			{ 0, 0, 1.f, 0},
			{ 0.001f, 0.001f, 0, 1.f}
		};

		VIEWPORT viewport;

		u32 lastTexParams = 0;
		u32 lastTexPalette = 0;
		u32 lastPolyAttr = 0;
		u32 lastViewport = 0xFFFFFFFF;

		sceGuSync(0, 0);
		sceGuStart(GU_DIRECT, gulist);

		sceGuSetMatrix(GU_PROJECTION, &_matrx);
		sceGuSetMatrix(GU_TEXTURE, &_matrx);
		sceGuSetMatrix(GU_MODEL, &_matrx);
		sceGuSetMatrix(GU_VIEW, &_matrx);

		sceGuDrawBufferList(GU_PSM_8888, (void*)renderTarget, 256);

		sceGuClearColor(0);
		sceGuClearDepth(0);
		sceGuClearStencil(0);

		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT | GU_STENCIL_BUFFER_BIT);

		sceKernelDcacheWritebackInvalidateAll();
		for(int i=0; i < polyCount; i++)
		{
			GFX3D_Clipper::TClippedPoly &clippedPoly = engine->clipper.clippedPolys[i];
			POLY &poly = *clippedPoly.poly;
			int type = clippedPoly.type;

			if (first || lastPolyAttr != poly.polyAttr)
			{
				lastPolyAttr = poly.polyAttr;
				
				SetupPoly(poly);

				polyAttr.setup(poly.polyAttr);
			}

			/*if (!polyAttr.isVisible(poly.backfacing) && !polyAttr.drawBackPlaneIntersectingPolys)
				continue;*/


			if (first ||lastTexParams != poly.texParam || lastTexPalette != poly.texPalette)
			{
				this->SetupTexture(poly);

				lastTexParams = poly.texParam;
				lastTexPalette = poly.texPalette;
				sceGuDrawArray(GUPrimitiveType[poly.vtxFormat], GU_TRANSFORM_3D, 0, 0, &vertices[VertListIndex]);
			}

			if (lastViewport != poly.viewport){
				viewport.decode(poly.viewport);
				lastViewport = poly.viewport;
			}

			first = false;


			for(int j=0;j<type;j++){
				VERT &vert = clippedPoly.clipVerts[j];
				Vertex &out = vertices[VertListIndex + j];

				ArraytoColor.a = 0xff;
				ArraytoColor.r = vert.color[0] << 3;
				ArraytoColor.g = vert.color[1] << 3;
				ArraytoColor.b = vert.color[2] << 3;

				out.col =  ArraytoColor.color;

				out.u = vert.u;
				out.v = vert.v;

				__asm__ volatile(				
					// load vert.x vert.y vert.z vert.w
					"lv.q			c000, 0 + %1\n"
					
					// add w to x and y
					"vadd.s		    S000, S000, S003\n"
					"vadd.s		    S001, S001, S003\n"
					"vadd.s		    S002, S002, S003\n"

					// 1/(w*2)
					"vadd.s		    S003, S003, S003\n"
					"vrcp.s		    S003, S003\n"

					// mul x and y z by 1/(w*2)
					"vscl.t		    c000, c000, S003\n"

					//viewport transform
					"ulv.q			c100, 0 + %2\n"
					"vi2f.q 	    c100, c100, 0\n"

					"vmul.p 	    c000, c000, c102\n"
					"vadd.p 	    c000, c000, c100\n"

					"viim.s 	    S100, 192\n"
					"vsub.s 	    S001, S100, S001\n"

					// save x and y z
					"sv.s			S000, 0 + %0\n"
					"sv.s			S001, 4 + %0\n"
					"sv.s			S002, 8 + %0\n"

					: "=m"(out.x)
					: "m"(vert.x), "m"(viewport.x)
					: "memory"
				);
			}

			sceGuDrawArray(GUPrimitiveType[poly.vtxFormat], GU_TEXTURE_32BITF|GU_COLOR_8888|GU_VERTEX_32BITF|GU_TRANSFORM_2D, type, 0, &vertices[VertListIndex]);
			VertListIndex += type;
		}
		
		sceGuFinishId(1);	
	}

};

static SoftRasterizerEngine mainSoftRasterizer;
static RasterizerUnit<true> rasterizerUnit;

void GU_callback(int i){
	if (i == 1) memcpy((u32*)&_screen[0], (u32*)(sceGeEdramGetAddr() + (int)0x44000),  192 * 256 * 4);
}

static char SoftRastInit(void)
{
	char result = Default3D_Init();
	if (result == 0)
	{
		return result;
	}

	rasterizerUnit.vertices = (struct Vertex*)sceGuGetMemory(VERTLIST_SIZE * sizeof(struct Vertex));
	rasterizerUnit.engine = &mainSoftRasterizer;
	memset(&rasterizerUnit.vertices[0], 0, VERTLIST_SIZE * sizeof(struct Vertex));

	sceGuSetCallback(GU_CALLBACK_FINISH, GU_callback);


	TexCache_Reset();
	return result;
}

static void SoftRastReset()
{
	softRastHasNewData = false;
	
	Default3D_Reset();
}

static void SoftRastClose()
{
	softRastHasNewData = false;
	
	Default3D_Close();
}

static void SoftRastVramReconfigureSignal()
{
	Default3D_VramReconfigureSignal();
}

static void SoftRastConvertFramebuffer(){ }


SoftRasterizerEngine::SoftRasterizerEngine()
{
	clipper.clippedPolys = new GFX3D_Clipper::TClippedPoly[POLYLIST_SIZE];
}



void SoftRasterizerEngine::performClipping()
{
	clipper.reset();

	const size_t polyCount = polylist->count;

	for (size_t i = 0; i < polyCount; i++)
	{
		POLY* poly = &polylist->list[indexlist->list[i]];
		VERT* verts[4] = {
			&vertlist->list[poly->vertIndexes[0]],
			&vertlist->list[poly->vertIndexes[1]],
			&vertlist->list[poly->vertIndexes[2]],
			poly->type == 4
				? &vertlist->list[poly->vertIndexes[3]]
				: NULL
		};
		clipper.clipPoly<false>(poly,verts);

		/*const int n = poly->type - 1;

		//move that inside the clipper (vfpu? maybe)
		float facing = (verts[0]->y + verts[n]->y) * (verts[0]->x - verts[n]->x)
					 + (verts[1]->y + verts[0]->y) * (verts[1]->x - verts[0]->x)
					 + (verts[2]->y + verts[1]->y) * (verts[2]->x - verts[1]->x);

		for(int j = 2; j < n; j++)
			facing += (verts[j+1]->y + verts[j]->y) * (verts[j+1]->x - verts[j]->x);
		
		poly->backfacing = (facing < 0);*/
	}

	clippedPolyCounter = clipper.clippedPolyCounter;
}


static void SoftRastRender()
{
	mainSoftRasterizer.polylist = gfx3d.polylist;
	mainSoftRasterizer.vertlist = gfx3d.vertlist;
	mainSoftRasterizer.indexlist = &gfx3d.indexlist;
	mainSoftRasterizer.width = GFX3D_FRAMEBUFFER_WIDTH;
	mainSoftRasterizer.height = GFX3D_FRAMEBUFFER_HEIGHT;
	
	softRastHasNewData = true;

	mainSoftRasterizer.performClipping();

	rasterizerUnit.mainLoop();
}

static void SoftRastRenderFinish()
{
	if (!softRastHasNewData) return;
	
	TexCache_EvictFrame();
	
	softRastHasNewData = false;
}

GPU3DInterface gpu3DRasterize = {
	"SoftRasterizer",
	SoftRastInit,
	SoftRastReset,
	SoftRastClose,
	SoftRastRender,
	SoftRastRenderFinish,
	SoftRastVramReconfigureSignal
};
