/*
	Copyright (C) 2009-2010 DeSmuME team

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

#ifndef _RASTERIZE_H_
#define _RASTERIZE_H_

#include "render3D.h"
#include "gfx3d.h"

extern  GPU3DInterface gpu3DRasterize;

extern volatile  u32 _screen[GFX3D_FRAMEBUFFER_WIDTH * GFX3D_FRAMEBUFFER_HEIGHT];

class TexCacheItem;

class SoftRasterizerEngine
{
public:
	SoftRasterizerEngine();

	void performClipping();

	GFX3D_Clipper clipper;
	GFX3D_Clipper::TClippedPoly * clippedPolys;
	int clippedPolyCounter;

	POLYLIST* polylist;
	VERTLIST* vertlist;
	INDEXLIST* indexlist;
	int width, height;
};


#endif