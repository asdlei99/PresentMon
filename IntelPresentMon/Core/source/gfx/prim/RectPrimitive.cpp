// Copyright (C) 2022 Intel Corporation
// SPDX-License-Identifier: MIT
#include "RectPrimitive.h"
#include <Core/source/win/WinAPI.h>
#include <d2d1_3.h>
#include "../base/InternalGeometry.h"
#include <Core/source/infra/log/Logging.h>

namespace p2c::gfx::prim
{
	RectPrimitive::RectPrimitive(const Rect& rect, std::shared_ptr<BrushPrimitive> pBrushPrim_, Graphics& gfx)
		:
		pBrushPrim{ std::move(pBrushPrim_) },
		pBrush{ Brush(*pBrushPrim) }
	{
		p2chrlog << Factory(gfx).CreateRectangleGeometry(d2d::Rect{ rect }, &pGeometry);
		p2chrlog << Context(gfx).CreateFilledGeometryRealization(pGeometry.Get(), D2D1_DEFAULT_FLATTENING_TOLERANCE, &pRealization);
	}

	RectPrimitive::~RectPrimitive() {}

	void RectPrimitive::Draw(Graphics& gfx) const
	{
		Context(gfx).DrawGeometryRealization(pRealization.Get(), pBrush.Get());
	}
}