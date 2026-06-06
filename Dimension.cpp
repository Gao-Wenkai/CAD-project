#include "pch.h"
#include "Dimension.h"
#include <cmath>
#include <algorithm>

IMPLEMENT_SERIAL(CDimLinearEntity, CEntity, 1)
IMPLEMENT_SERIAL(CDimAngularEntity, CEntity, 1)
IMPLEMENT_SERIAL(CDimRadiusEntity, CEntity, 1)
IMPLEMENT_SERIAL(CDimDiamEntity, CEntity, 1)
IMPLEMENT_SERIAL(CDimArcLengthEntity, CEntity, 1)

CDimLinearEntity::CDimLinearEntity() : m_pt1(0,0), m_pt2(0,0), m_mode(DIM_ALIGNED), m_offset(0.0), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_TEXT; }
CDimLinearEntity::CDimLinearEntity(CPoint p1, CPoint p2, DimMode mode, double offset, CPoint place) : m_pt1(p1), m_pt2(p2), m_mode(mode), m_offset(offset), m_place(place), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_TEXT; }

void CDimLinearEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
	if (!m_bVisible) return;
	CPoint a = m_pt1, b = m_pt2;
	// projection depending on mode
	if (m_mode == DIM_HORIZONTAL) {
		b.y = a.y;
	} else if (m_mode == DIM_VERTICAL) {
		b.x = a.x;
	}
	// compute direction in world coords
	double wx = (double)(b.x - a.x);
	double wy = (double)(b.y - a.y);
	double wlen = sqrt(wx*wx + wy*wy);
	CPoint aDimWorld = a, bDimWorld = b;
	// choose an offset in screen pixels then convert to world units
	double screenOffset = 15.0; // pixels
	double offWorld = 1.0;
	// prefer explicit m_offset (set by user on placement); if zero use default screen pixel offset
	if (fabs(m_offset) > 1e-6) {
		offWorld = m_offset;
	} else if (scale > 0.0) offWorld = screenOffset / scale;
	if (wlen > 1e-6) {
		// For H/V modes, compute the placed dimension segment using placement coordinate
		if (m_mode == DIM_HORIZONTAL || m_mode == DIM_VERTICAL) {
			CPoint place = (m_place.x == 0 && m_place.y == 0) ? CPoint((a.x + b.x)/2, (a.y + b.y)/2) : m_place;
			int x1 = a.x, x2 = b.x; int y1 = a.y, y2 = b.y;
			// ensure ordering
			int left = min(x1, x2), right = max(x1, x2);
			int top = min(y1, y2), bottom = max(y1, y2);
			if (m_mode == DIM_HORIZONTAL) {
				// dimension line from (left, place.y) to (right, place.y)
				aDimWorld = CPoint(left, place.y);
				bDimWorld = CPoint(right, place.y);
			} else {
				// vertical: from (place.x, top) to (place.x, bottom)
				aDimWorld = CPoint(place.x, top);
				bDimWorld = CPoint(place.x, bottom);
			}
		} else {
			// direction along projected (or axis-projected) line
			double ux = wx; double uy = wy; // not normalized
			double ulen2 = ux*ux + uy*uy;
			// unit normal
			double nx = -uy / sqrt(ulen2);
			double ny = ux / sqrt(ulen2);
			// base point on dimension line (aProj shifted by offset)
			CPoint aProj = a;
			CPoint bProj = b;
			// p0 = aProj + n * offWorld
			double p0x = (double)aProj.x + nx * offWorld;
			double p0y = (double)aProj.y + ny * offWorld;
			// for each original point, compute perpendicular foot onto the dim line
			auto projectToLine = [&](CPoint p)->CPoint {
				double vx = (double)(p.x) - p0x;
				double vy = (double)(p.y) - p0y;
				double t = (vx * ux + vy * uy) / ulen2; // scalar along u
				double ix = p0x + ux * t;
				double iy = p0y + uy * t;
				return CPoint((int)floor(ix + 0.5), (int)floor(iy + 0.5));
			};
			aDimWorld = projectToLine(a);
			bDimWorld = projectToLine(b);
		}
	} else {
		// degenerate: fallback small offset in +y world
		aDimWorld.y += (int)offWorld; bDimWorld.y += (int)offWorld;
	}
	// keep original measured points for correct extension line origins
	CPoint origA = m_pt1, origB = m_pt2;
	CPoint sA = ToScreen(origA, scale, offset);
	CPoint sB = ToScreen(origB, scale, offset);
	CPoint sAD = ToScreen(aDimWorld, scale, offset);
	CPoint sBD = ToScreen(bDimWorld, scale, offset);
	CPen pen(m_nLineStyle, m_nLineWidth, m_color);
	CPen* pOld = pDC->SelectObject(&pen);
	// draw extension/connection lines
	if (m_mode == DIM_HORIZONTAL || m_mode == DIM_VERTICAL) {
		// For H/V modes, connect each original point vertically/horizontally to the dim line at user's placement
		if (m_mode == DIM_HORIZONTAL) {
			CPoint conn1(origA.x, aDimWorld.y); CPoint conn2(origB.x, bDimWorld.y);
			pDC->MoveTo(sA); pDC->LineTo(ToScreen(conn1, scale, offset));
			pDC->MoveTo(sB); pDC->LineTo(ToScreen(conn2, scale, offset));
		} else {
			CPoint conn1(aDimWorld.x, origA.y); CPoint conn2(bDimWorld.x, origB.y);
			pDC->MoveTo(sA); pDC->LineTo(ToScreen(conn1, scale, offset));
			pDC->MoveTo(sB); pDC->LineTo(ToScreen(conn2, scale, offset));
		}
	} else {
		// draw extension lines from measured points to dimension line (aligned mode)
		pDC->MoveTo(sA); pDC->LineTo(sAD);
		pDC->MoveTo(sB); pDC->LineTo(sBD);
	}
	// draw main dimension line between offsets
	pDC->MoveTo(sAD); pDC->LineTo(sBD);
	// draw arrowheads at both ends
	int arrowLen = max(8, (int)(8 * scale));
	auto drawArrow = [&](CPoint pt, CPoint toward) {
		double dx = (double)(toward.x - pt.x);
		double dy = (double)(toward.y - pt.y);
		double d = sqrt(dx*dx + dy*dy);
		if (d < 1e-6) return;
		double ux = dx / d, uy = dy / d;
		double ang = atan2(uy, ux);
		double ang1 = ang + M_PI * 3.0 / 8.0; // 67.5 deg
		double ang2 = ang - M_PI * 3.0 / 8.0;
		CPoint p1((int)floor(pt.x + cos(ang1) * arrowLen + 0.5), (int)floor(pt.y + sin(ang1) * arrowLen + 0.5));
		CPoint p2((int)floor(pt.x + cos(ang2) * arrowLen + 0.5), (int)floor(pt.y + sin(ang2) * arrowLen + 0.5));
		pDC->MoveTo(pt); pDC->LineTo(p1);
		pDC->MoveTo(pt); pDC->LineTo(p2);
	};
	// arrow at start points toward bDim
	drawArrow(sAD, sBD);
	// arrow at end points toward aDim
	drawArrow(sBD, sAD);
	// for aligned mode we already drew extension lines earlier; for H/V the projections
	// and their connection lines were drawn above, so no extra handling needed here.
	// draw text (length) near middle of dimension line, offset slightly outward
	double len;
	if (m_mode == DIM_HORIZONTAL) len = fabs((double)(m_pt2.x - m_pt1.x));
	else if (m_mode == DIM_VERTICAL) len = fabs((double)(m_pt2.y - m_pt1.y));
	else len = Distance(m_pt1, m_pt2);
	CString txt; txt.Format(L"%.2f", len);
	CPoint mid((sAD.x + sBD.x)/2, (sAD.y + sBD.y)/2);
	// if explicit text placement provided (world coords), use it
	if (!(m_ptText.x == 0 && m_ptText.y == 0)) {
		CPoint st = ToScreen(m_ptText, scale, offset);
		mid = st;
	}
	int textOffset = max(6, (int)(6 * scale));
	// offset text perpendicular in screen coords: use normal from sAD->sBD
	double sx = (double)(sBD.x - sAD.x);
	double sy = (double)(sBD.y - sAD.y);
	double slen = sqrt(sx*sx + sy*sy);
	if (slen > 1e-6) {
		double nxs = -sy / slen;
		double nys = sx / slen;
		mid.x = (int)floor(mid.x + nxs * textOffset + 0.5);
		mid.y = (int)floor(mid.y + nys * textOffset + 0.5);
	}
	CFont font; int h = max(8, (int)(12*scale)); font.CreatePointFont(h*10, _T("Consolas"));
	CFont* oldF = pDC->SelectObject(&font);
	COLORREF oldC = pDC->SetTextColor(m_color);
	int oldBk = pDC->SetBkMode(TRANSPARENT);
	if (m_bTextPlaced)
		pDC->TextOutW(mid.x+4, mid.y+4, txt);
	pDC->SetBkMode(oldBk); pDC->SetTextColor(oldC); pDC->SelectObject(oldF); font.DeleteObject();
	pDC->SelectObject(pOld);
}

bool CDimLinearEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
	CPoint a = m_pt1, b = m_pt2;
	if (m_mode == DIM_HORIZONTAL) b.y = a.y;
	else if (m_mode == DIM_VERTICAL) b.x = a.x;
	double wx = (double)(b.x - a.x);
	double wy = (double)(b.y - a.y);
	double wlen = sqrt(wx*wx + wy*wy);
	double screenOffset = 15.0;
	double offWorld = 1.0;
	if (fabs(m_offset) > 1e-6) offWorld = m_offset;
	else if (scale > 0.0) offWorld = screenOffset / scale;
	CPoint aDim = a, bDim = b;
	if (wlen > 1e-6) {
		double ux = wx; double uy = wy; double ulen2 = ux*ux + uy*uy;
		double nx = -uy / sqrt(ulen2);
		double ny = ux / sqrt(ulen2);
		double p0x = (double)a.x + nx * offWorld;
		double p0y = (double)a.y + ny * offWorld;
		auto projectToLine = [&](CPoint p)->CPoint {
			double vx = (double)(p.x) - p0x; double vy = (double)(p.y) - p0y;
			double t = (vx * ux + vy * uy) / ulen2;
			double ix = p0x + ux * t; double iy = p0y + uy * t;
			return CPoint((int)floor(ix + 0.5), (int)floor(iy + 0.5));
		};
		aDim = projectToLine(a);
		bDim = projectToLine(b);
	} else {
		aDim.y += (int)offWorld; bDim.y += (int)offWorld;
	}
	CPoint s1 = ToScreen(aDim, scale, offset);
	CPoint s2 = ToScreen(bDim, scale, offset);
	return PointToLineDistance(pt, s1, s2) <= 6.0;
}

void CDimLinearEntity::Move(double dx, double dy) { m_pt1.x += (int)dx; m_pt1.y += (int)dy; m_pt2.x += (int)dx; m_pt2.y += (int)dy; }
CRect CDimLinearEntity::GetBounds() { CRect rc(m_pt1, m_pt2); rc.NormalizeRect(); rc.InflateRect(6,6); return rc; }
CEntity* CDimLinearEntity::Clone() const { CDimLinearEntity* p = new CDimLinearEntity(m_pt1, m_pt2, m_mode, m_offset); p->m_color = m_color; p->m_nLineStyle = m_nLineStyle; p->m_nLineWidth = m_nLineWidth; p->m_ptText = m_ptText; p->m_bTextPlaced = m_bTextPlaced; return p; }
void CDimLinearEntity::Serialize(CArchive& ar) { CEntity::Serialize(ar); if (ar.IsStoring()) ar << m_pt1.x << m_pt1.y << m_pt2.x << m_pt2.y << (int)m_mode << m_offset << m_ptText.x << m_ptText.y << m_bTextPlaced; else { ar >> m_pt1.x >> m_pt1.y >> m_pt2.x >> m_pt2.y; int m; ar >> m; m_mode = (DimMode)m; ar >> m_offset; ar >> m_ptText.x >> m_ptText.y; ar >> m_bTextPlaced; } }

CDimAngularEntity::CDimAngularEntity() : m_ptCenter(0,0), m_pt1(0,0), m_pt2(0,0), m_ptPlace(0,0), m_ptText(0,0), m_bTextPlaced(true), m_ang1(0), m_ang2(0), m_v1x(1.0), m_v1y(0.0), m_v2x(0.0), m_v2y(1.0) { m_Type = ENT_TEXT; }
CDimAngularEntity::CDimAngularEntity(CPoint center, CPoint p1, CPoint p2, CPoint place) : m_ptCenter(center), m_pt1(p1), m_pt2(p2), m_ptPlace(place), m_ptText(0,0), m_bTextPlaced(true), m_ang1(0), m_ang2(0), m_v1x(1.0), m_v1y(0.0), m_v2x(0.0), m_v2y(1.0) {
	m_Type = ENT_TEXT;
	m_ang1 = atan2((double)(m_pt1.y - m_ptCenter.y), (double)(m_pt1.x - m_ptCenter.x));
	m_ang2 = atan2((double)(m_pt2.y - m_ptCenter.y), (double)(m_pt2.x - m_ptCenter.x));
	// initialize unit vectors from angles
	m_v1x = cos(m_ang1); m_v1y = sin(m_ang1);
	m_v2x = cos(m_ang2); m_v2y = sin(m_ang2);
}

CDimAngularEntity::CDimAngularEntity(CPoint center, CPoint p1, CPoint p2, CPoint place, double ang1, double ang2) : m_ptCenter(center), m_pt1(p1), m_pt2(p2), m_ptPlace(place), m_ptText(0,0), m_bTextPlaced(true), m_ang1(ang1), m_ang2(ang2) {
	m_Type = ENT_TEXT;
	m_v1x = cos(m_ang1); m_v1y = sin(m_ang1);
	m_v2x = cos(m_ang2); m_v2y = sin(m_ang2);
}

void CDimAngularEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
	if (!m_bVisible) return;
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	double a1 = m_ang1;
	double a2 = m_ang2;
	// normalize angles to [0, 2pi)
	auto norm = [](double ang){ while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI; return ang; };
	double start = norm(a1), end = norm(a2);
	// CCW sweep from start to end
	double sweep = end - start; if (sweep < 0) sweep += 2*M_PI;
	// ensure the Arc() call will draw the intended arc (minor when sweep<=PI, major otherwise)
	// compute intended mid-angle and if Arc would draw the opposite arc, swap start/end
	auto midAngle = norm(start + sweep / 2.0);
	// compute midSweep: angle from start to midAngle CCW
	double midSweep = midAngle - start; if (midSweep < 0) midSweep += 2*M_PI;
	// If computed midSweep is larger than sweep, that means the arc drawn by Arc() would not
	// pass through the intended mid point; swap start/end to make Arc() draw the intended arc.
	if (midSweep > sweep) {
		double tmp = start; start = end; end = tmp;
		sweep = 2*M_PI - sweep;
		// recompute midAngle for the new start/end
		midAngle = norm(start + sweep / 2.0);
	}
	int r = max(16, (int)(20*scale));
	// if user provided a placement point, adjust radius so arc passes through that point
	if (!(m_ptPlace.x == 0 && m_ptPlace.y == 0)) {
		// compute screen radius from center to placement
		CPoint scPlace = ToScreen(m_ptPlace, scale, offset);
		int rPlace = (int)floor(sqrt((scPlace.x - sc.x)*(scPlace.x - sc.x) + (scPlace.y - sc.y)*(scPlace.y - sc.y)) + 0.5);
		if (rPlace > 4) r = rPlace;
	}
	CRect rc(sc.x - r, sc.y - r, sc.x + r, sc.y + r);
	CPen pen(m_nLineStyle, m_nLineWidth, m_color);
	CPen* old = pDC->SelectObject(&pen);
	// After possibly swapping start/end above, compute ray endpoints from those final angles
	double ux_s = cos(start), uy_s = sin(start);
	double ux_e = cos(end),   uy_e = sin(end);
	CPoint ps((int)floor(sc.x + r * ux_s + 0.5), (int)floor(sc.y + r * uy_s + 0.5));
	CPoint pe((int)floor(sc.x + r * ux_e + 0.5), (int)floor(sc.y + r * uy_e + 0.5));
	// draw the two rays using final angles so they precisely align with the arc endpoints
	pDC->MoveTo(sc); pDC->LineTo(ps);
	pDC->MoveTo(sc); pDC->LineTo(pe);
	// draw the arc by sampling points along the intended CCW sweep so we control
	// whether the minor (inner) or major (outer) arc is drawn regardless of Arc()
	if (fabs(sweep) > 1e-6) {
		// choose steps such that roughly one segment per ~5 degrees
		int steps = max(12, (int)ceil(sweep / (M_PI / 36.0)));
		for (int i = 0; i <= steps; ++i) {
			// when sampling between start and end we still compute points by angle to get smooth arc
			double ang = start + sweep * (double)i / (double)steps;
			// normalize ang to avoid drift
			while (ang < 0) ang += 2*M_PI; while (ang >= 2*M_PI) ang -= 2*M_PI;
			CPoint p((int)floor(sc.x + r * cos(ang) + 0.5), (int)floor(sc.y + r * sin(ang) + 0.5));
			if (i == 0) pDC->MoveTo(p);
			else pDC->LineTo(p);
		}
	}
	// angle to display (use the CCW sweep as the chosen angle)
	double angle = sweep; if (angle > 2*M_PI) angle = fmod(angle, 2*M_PI);
	double deg = angle * 180.0 / M_PI;
	CString txt; txt.Format(L"%.1f deg", deg);
	// place text in inner (short) region for small angles, and in outer region for large angles
	// compute mid-angle of the drawn arc (CCW)
	double mid = midAngle; // already normalized and adjusted above
	// choose text radius so it's clearly inside for inner angles and clearly outside for outer angles
	// use proportional placement to be robust across scales
	double textR;
	double minInner = max(8.0, r * 0.25);   // minimal inner radius
	double minOuter = max(8.0, r * 0.25);   // minimal offset for outer
	if (sweep <= M_PI) {
		// inner (short) arc: place text well inside the arc (closer to center)
		textR = max(minInner, r * 0.55);
	} else {
		// outer (long) arc: place text clearly outside the arc radius
		textR = r + max(minOuter, r * 0.45);
	}
	CPoint txtPt((int)(sc.x + textR * cos(mid)), (int)(sc.y + textR * sin(mid)));
	if (!(m_ptText.x == 0 && m_ptText.y == 0)) {
		txtPt = ToScreen(m_ptText, scale, offset);
	}
	CFont font; int h = max(8, (int)(10*scale)); font.CreatePointFont(h*10, _T("Consolas"));
	CFont* oldF = pDC->SelectObject(&font);
	COLORREF oldC = pDC->SetTextColor(m_color);
	int oldBk = pDC->SetBkMode(TRANSPARENT);
	if (m_bTextPlaced)
		pDC->TextOutW(txtPt.x, txtPt.y, txt);
	pDC->SetBkMode(oldBk); pDC->SetTextColor(oldC); pDC->SelectObject(oldF); font.DeleteObject();
	pDC->SelectObject(old);
}

bool CDimAngularEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	double d = Distance(pt, sc);
	return d <= 30.0 && d >= 6.0;
}

void CDimAngularEntity::Move(double dx, double dy) { m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy; m_pt1.x += (int)dx; m_pt1.y += (int)dy; m_pt2.x += (int)dx; m_pt2.y += (int)dy; }
CRect CDimAngularEntity::GetBounds() { CRect rc(m_ptCenter.x-30, m_ptCenter.y-30, m_ptCenter.x+30, m_ptCenter.y+30); return rc; }
CEntity* CDimAngularEntity::Clone() const { CDimAngularEntity* p = new CDimAngularEntity(m_ptCenter, m_pt1, m_pt2, m_ptPlace, m_ang1, m_ang2); p->m_color = m_color; p->m_nLineStyle = m_nLineStyle; p->m_nLineWidth = m_nLineWidth; p->m_v1x = m_v1x; p->m_v1y = m_v1y; p->m_v2x = m_v2x; p->m_v2y = m_v2y; p->m_ptText = m_ptText; p->m_bTextPlaced = m_bTextPlaced; return p; }
void CDimAngularEntity::Serialize(CArchive& ar) { CEntity::Serialize(ar); if (ar.IsStoring()) ar << m_ptCenter.x << m_ptCenter.y << m_pt1.x << m_pt1.y << m_pt2.x << m_pt2.y << m_ptPlace.x << m_ptPlace.y << m_ang1 << m_ang2 << m_v1x << m_v1y << m_v2x << m_v2y << m_ptText.x << m_ptText.y << m_bTextPlaced; else { ar >> m_ptCenter.x >> m_ptCenter.y >> m_pt1.x >> m_pt1.y >> m_pt2.x >> m_pt2.y >> m_ptPlace.x >> m_ptPlace.y >> m_ang1 >> m_ang2 >> m_v1x >> m_v1y >> m_v2x >> m_v2y >> m_ptText.x >> m_ptText.y >> m_bTextPlaced; } }

// Transform helpers
void CDimLinearEntity::Rotate(CPoint base, double angle) { m_pt1 = RotatePoint(m_pt1, base, angle); m_pt2 = RotatePoint(m_pt2, base, angle); }
void CDimLinearEntity::Scale(CPoint base, double factor) { m_pt1 = ScalePoint(m_pt1, base, factor); m_pt2 = ScalePoint(m_pt2, base, factor); }
void CDimLinearEntity::Mirror(CPoint p1, CPoint p2) { m_pt1 = MirrorPoint(m_pt1, p1, p2); m_pt2 = MirrorPoint(m_pt2, p1, p2); }

void CDimAngularEntity::Rotate(CPoint base, double angle) { m_ptCenter = RotatePoint(m_ptCenter, base, angle); m_pt1 = RotatePoint(m_pt1, base, angle); m_pt2 = RotatePoint(m_pt2, base, angle); }
void CDimAngularEntity::Scale(CPoint base, double factor) { m_ptCenter = ScalePoint(m_ptCenter, base, factor); m_pt1 = ScalePoint(m_pt1, base, factor); m_pt2 = ScalePoint(m_pt2, base, factor); }
void CDimAngularEntity::Mirror(CPoint p1, CPoint p2) { m_ptCenter = MirrorPoint(m_ptCenter, p1, p2); m_pt1 = MirrorPoint(m_pt1, p1, p2); m_pt2 = MirrorPoint(m_pt2, p1, p2); }

CDimRadiusEntity::CDimRadiusEntity() : m_ptCenter(0,0), m_nRadius(0), m_dAngle(0.0), m_ptLeaderEnd(0,0), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_DIM_RADIUS; }

CDimRadiusEntity::CDimRadiusEntity(CPoint center, int radius, double angle) : m_ptCenter(center), m_nRadius(radius), m_dAngle(angle), m_ptText(0,0), m_bTextPlaced(true) {
	m_Type = ENT_DIM_RADIUS;
	m_ptLeaderEnd = CPoint((int)floor(center.x + radius * cos(angle) + 0.5), (int)floor(center.y + radius * sin(angle) + 0.5));
}

void CDimRadiusEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
	if (!m_bVisible) return;
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	CPoint sle = ToScreen(m_ptLeaderEnd, scale, offset);
	CPen pen(m_nLineStyle, m_nLineWidth, m_color);
	CPen* pOld = pDC->SelectObject(&pen);
	pDC->MoveTo(sc); pDC->LineTo(sle);
	double dx = (double)(sle.x - sc.x), dy = (double)(sle.y - sc.y);
	double d = sqrt(dx*dx + dy*dy);
	if (d > 1e-6) {
		double ux = dx / d, uy = dy / d;
		int arrowLen = max(8, (int)(8 * scale));
		double ang = atan2(uy, ux);
		double ang1 = ang + M_PI * 3.0 / 8.0, ang2 = ang - M_PI * 3.0 / 8.0;
		CPoint p1((int)floor(sle.x - cos(ang1) * arrowLen + 0.5), (int)floor(sle.y - sin(ang1) * arrowLen + 0.5));
		CPoint p2((int)floor(sle.x - cos(ang2) * arrowLen + 0.5), (int)floor(sle.y - sin(ang2) * arrowLen + 0.5));
		pDC->MoveTo(sle); pDC->LineTo(p1);
		pDC->MoveTo(sle); pDC->LineTo(p2);
	}
	CString txt; txt.Format(L"%.2f", (double)m_nRadius);
	CPoint txtPt;
	if (!(m_ptText.x == 0 && m_ptText.y == 0)) {
		txtPt = ToScreen(m_ptText, scale, offset);
	} else {
		CPoint mid((sc.x + sle.x)/2, (sc.y + sle.y)/2);
		int textOffset = max(6, (int)(6 * scale));
		double sx = (double)(sle.x - sc.x), sy = (double)(sle.y - sc.y);
		double slen = sqrt(sx*sx + sy*sy);
		if (slen > 1e-6) {
			double nxs = -sy / slen, nys = sx / slen;
			mid.x = (int)floor(mid.x + nxs * textOffset + 0.5);
			mid.y = (int)floor(mid.y + nys * textOffset + 0.5);
		}
		txtPt = mid;
	}
	CFont font; int h = max(8, (int)(12*scale)); font.CreatePointFont(h*10, _T("Consolas"));
	CFont* oldF = pDC->SelectObject(&font);
	COLORREF oldC = pDC->SetTextColor(m_color);
	int oldBk = pDC->SetBkMode(TRANSPARENT);
	if (m_bTextPlaced) pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
	pDC->SetBkMode(oldBk); pDC->SetTextColor(oldC); pDC->SelectObject(oldF); font.DeleteObject();
	pDC->SelectObject(pOld);
}

bool CDimRadiusEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	CPoint sle = ToScreen(m_ptLeaderEnd, scale, offset);
	return PointToLineDistance(pt, sc, sle) <= 6.0;
}

void CDimRadiusEntity::Move(double dx, double dy) { m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy; m_ptLeaderEnd.x += (int)dx; m_ptLeaderEnd.y += (int)dy; m_ptText.x += (int)dx; m_ptText.y += (int)dy; }

CRect CDimRadiusEntity::GetBounds() { CRect rc(m_ptCenter, m_ptLeaderEnd); rc.NormalizeRect(); rc.InflateRect(30,30); return rc; }

CEntity* CDimRadiusEntity::Clone() const {
	CDimRadiusEntity* p = new CDimRadiusEntity(m_ptCenter, m_nRadius, m_dAngle);
	p->m_ptLeaderEnd = m_ptLeaderEnd; p->m_ptText = m_ptText; p->m_bTextPlaced = m_bTextPlaced;
	p->m_color = m_color; p->m_nLineStyle = m_nLineStyle; p->m_nLineWidth = m_nLineWidth;
	return p;
}

void CDimRadiusEntity::Rotate(CPoint base, double angle) { m_ptCenter = RotatePoint(m_ptCenter, base, angle); m_ptLeaderEnd = RotatePoint(m_ptLeaderEnd, base, angle); m_ptText = RotatePoint(m_ptText, base, angle); m_dAngle += angle; }

void CDimRadiusEntity::Scale(CPoint base, double factor) { m_ptCenter = ScalePoint(m_ptCenter, base, factor); m_ptLeaderEnd = ScalePoint(m_ptLeaderEnd, base, factor); m_ptText = ScalePoint(m_ptText, base, factor); m_nRadius = (int)(m_nRadius * factor + 0.5); }

void CDimRadiusEntity::Mirror(CPoint p1, CPoint p2) {
	m_ptCenter = MirrorPoint(m_ptCenter, p1, p2); m_ptLeaderEnd = MirrorPoint(m_ptLeaderEnd, p1, p2);
	m_ptText = MirrorPoint(m_ptText, p1, p2);
	m_dAngle = atan2((double)(m_ptLeaderEnd.y - m_ptCenter.y), (double)(m_ptLeaderEnd.x - m_ptCenter.x));
}

void CDimRadiusEntity::Serialize(CArchive& ar) {
	CEntity::Serialize(ar);
	if (ar.IsStoring()) ar << m_ptCenter.x << m_ptCenter.y << m_nRadius << m_dAngle << m_ptLeaderEnd.x << m_ptLeaderEnd.y << m_ptText.x << m_ptText.y << m_bTextPlaced;
	else { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nRadius >> m_dAngle >> m_ptLeaderEnd.x >> m_ptLeaderEnd.y >> m_ptText.x >> m_ptText.y >> m_bTextPlaced; }
}

CDimDiamEntity::CDimDiamEntity() : m_ptCenter(0,0), m_nRadius(0), m_dAngle(0.0), m_ptEnd1(0,0), m_ptEnd2(0,0), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_DIM_DIAMETER; }

CDimDiamEntity::CDimDiamEntity(CPoint center, int radius, double angle) : m_ptCenter(center), m_nRadius(radius), m_dAngle(angle), m_ptText(0,0), m_bTextPlaced(true) {
	m_Type = ENT_DIM_DIAMETER;
	m_ptEnd1 = CPoint((int)floor(center.x - radius * cos(angle) + 0.5), (int)floor(center.y - radius * sin(angle) + 0.5));
	m_ptEnd2 = CPoint((int)floor(center.x + radius * cos(angle) + 0.5), (int)floor(center.y + radius * sin(angle) + 0.5));
}

void CDimDiamEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
	if (!m_bVisible) return;
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	CPoint se1 = ToScreen(m_ptEnd1, scale, offset);
	CPoint se2 = ToScreen(m_ptEnd2, scale, offset);
	CPen pen(m_nLineStyle, m_nLineWidth, m_color);
	CPen* pOld = pDC->SelectObject(&pen);
	pDC->MoveTo(se1); pDC->LineTo(se2);
	int arrowLen = max(8, (int)(8 * scale));
	auto drawArrow = [&](CPoint tip, CPoint inward) {
		double dx = (double)(inward.x - tip.x), dy = (double)(inward.y - tip.y);
		double d = sqrt(dx*dx + dy*dy);
		if (d < 1e-6) return;
		double ux = dx / d, uy = dy / d;
		double ang = atan2(uy, ux);
		double a1 = ang + M_PI * 3.0 / 8.0, a2 = ang - M_PI * 3.0 / 8.0;
		CPoint p1((int)floor(tip.x + cos(a1) * arrowLen + 0.5), (int)floor(tip.y + sin(a1) * arrowLen + 0.5));
		CPoint p2((int)floor(tip.x + cos(a2) * arrowLen + 0.5), (int)floor(tip.y + sin(a2) * arrowLen + 0.5));
		pDC->MoveTo(tip); pDC->LineTo(p1);
		pDC->MoveTo(tip); pDC->LineTo(p2);
	};
	drawArrow(se1, se2);
	drawArrow(se2, se1);
	int diam = m_nRadius * 2;
	CString txt; txt.Format(L"%.2f", (double)diam);
	CPoint txtPt;
	if (!(m_ptText.x == 0 && m_ptText.y == 0)) {
		txtPt = ToScreen(m_ptText, scale, offset);
	} else {
		CPoint mid((se1.x + se2.x)/2, (se1.y + se2.y)/2);
		int textOffset = max(6, (int)(6 * scale));
		double sx = (double)(se2.x - se1.x), sy = (double)(se2.y - se1.y);
		double slen = sqrt(sx*sx + sy*sy);
		if (slen > 1e-6) {
			double nxs = -sy / slen, nys = sx / slen;
			mid.x = (int)floor(mid.x + nxs * textOffset + 0.5);
			mid.y = (int)floor(mid.y + nys * textOffset + 0.5);
		}
		txtPt = mid;
	}
	CFont font; int h = max(8, (int)(12*scale)); font.CreatePointFont(h*10, _T("Consolas"));
	CFont* oldF = pDC->SelectObject(&font);
	COLORREF oldC = pDC->SetTextColor(m_color);
	int oldBk = pDC->SetBkMode(TRANSPARENT);
	if (m_bTextPlaced) pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
	pDC->SetBkMode(oldBk); pDC->SetTextColor(oldC); pDC->SelectObject(oldF); font.DeleteObject();
	pDC->SelectObject(pOld);
}

bool CDimDiamEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
	CPoint se1 = ToScreen(m_ptEnd1, scale, offset);
	CPoint se2 = ToScreen(m_ptEnd2, scale, offset);
	return PointToLineDistance(pt, se1, se2) <= 6.0;
}

void CDimDiamEntity::Move(double dx, double dy) { m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy; m_ptEnd1.x += (int)dx; m_ptEnd1.y += (int)dy; m_ptEnd2.x += (int)dx; m_ptEnd2.y += (int)dy; m_ptText.x += (int)dx; m_ptText.y += (int)dy; }

CRect CDimDiamEntity::GetBounds() { CRect rc(m_ptEnd1, m_ptEnd2); rc.NormalizeRect(); rc.InflateRect(30,30); return rc; }

CEntity* CDimDiamEntity::Clone() const {
	CDimDiamEntity* p = new CDimDiamEntity(m_ptCenter, m_nRadius, m_dAngle);
	p->m_ptEnd1 = m_ptEnd1; p->m_ptEnd2 = m_ptEnd2; p->m_ptText = m_ptText; p->m_bTextPlaced = m_bTextPlaced;
	p->m_color = m_color; p->m_nLineStyle = m_nLineStyle; p->m_nLineWidth = m_nLineWidth;
	return p;
}

void CDimDiamEntity::Rotate(CPoint base, double angle) { m_ptCenter = RotatePoint(m_ptCenter, base, angle); m_ptEnd1 = RotatePoint(m_ptEnd1, base, angle); m_ptEnd2 = RotatePoint(m_ptEnd2, base, angle); m_ptText = RotatePoint(m_ptText, base, angle); m_dAngle += angle; }

void CDimDiamEntity::Scale(CPoint base, double factor) { m_ptCenter = ScalePoint(m_ptCenter, base, factor); m_ptEnd1 = ScalePoint(m_ptEnd1, base, factor); m_ptEnd2 = ScalePoint(m_ptEnd2, base, factor); m_ptText = ScalePoint(m_ptText, base, factor); m_nRadius = (int)(m_nRadius * factor + 0.5); }

void CDimDiamEntity::Mirror(CPoint p1, CPoint p2) {
	m_ptCenter = MirrorPoint(m_ptCenter, p1, p2); m_ptEnd1 = MirrorPoint(m_ptEnd1, p1, p2); m_ptEnd2 = MirrorPoint(m_ptEnd2, p1, p2);
	m_ptText = MirrorPoint(m_ptText, p1, p2);
	m_dAngle = atan2((double)(m_ptEnd2.y - m_ptEnd1.y), (double)(m_ptEnd2.x - m_ptEnd1.x));
}

void CDimDiamEntity::Serialize(CArchive& ar) {
	CEntity::Serialize(ar);
	if (ar.IsStoring()) ar << m_ptCenter.x << m_ptCenter.y << m_nRadius << m_dAngle << m_ptEnd1.x << m_ptEnd1.y << m_ptEnd2.x << m_ptEnd2.y << m_ptText.x << m_ptText.y << m_bTextPlaced;
	else { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nRadius >> m_dAngle >> m_ptEnd1.x >> m_ptEnd1.y >> m_ptEnd2.x >> m_ptEnd2.y >> m_ptText.x >> m_ptText.y >> m_bTextPlaced; }
}

CDimArcLengthEntity::CDimArcLengthEntity() : m_ptCenter(0,0), m_nArcRadius(0), m_nDimRadius(0), m_dStartAngle(0), m_dEndAngle(0), m_dSweep(0), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_DIM_ARCLENGTH; }

CDimArcLengthEntity::CDimArcLengthEntity(CPoint center, int arcRadius, int dimRadius, double startAngle, double endAngle, double sweep) : m_ptCenter(center), m_nArcRadius(arcRadius), m_nDimRadius(dimRadius), m_dStartAngle(startAngle), m_dEndAngle(endAngle), m_dSweep(sweep), m_ptText(0,0), m_bTextPlaced(true) { m_Type = ENT_DIM_ARCLENGTH; }

void CDimArcLengthEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
	if (!m_bVisible) return;
	CPen pen(m_nLineStyle, m_nLineWidth, m_color);
	CPen* pOld = pDC->SelectObject(&pen);

	// Compute arc endpoints in world coords
	CPoint arcStart((int)floor(m_ptCenter.x + m_nArcRadius * cos(m_dStartAngle) + 0.5), (int)floor(m_ptCenter.y + m_nArcRadius * sin(m_dStartAngle) + 0.5));
	CPoint arcEnd((int)floor(m_ptCenter.x + m_nArcRadius * cos(m_dEndAngle) + 0.5), (int)floor(m_ptCenter.y + m_nArcRadius * sin(m_dEndAngle) + 0.5));
	CPoint dimStart((int)floor(m_ptCenter.x + m_nDimRadius * cos(m_dStartAngle) + 0.5), (int)floor(m_ptCenter.y + m_nDimRadius * sin(m_dStartAngle) + 0.5));
	CPoint dimEnd((int)floor(m_ptCenter.x + m_nDimRadius * cos(m_dEndAngle) + 0.5), (int)floor(m_ptCenter.y + m_nDimRadius * sin(m_dEndAngle) + 0.5));

	CPoint sAS = ToScreen(arcStart, scale, offset);
	CPoint sAE = ToScreen(arcEnd, scale, offset);
	CPoint sDS = ToScreen(dimStart, scale, offset);
	CPoint sDE = ToScreen(dimEnd, scale, offset);
	CPoint sc = ToScreen(m_ptCenter, scale, offset);
	int sr = (int)(m_nDimRadius * scale);

	// Draw extension lines
	pDC->MoveTo(sAS); pDC->LineTo(sDS);
	pDC->MoveTo(sAE); pDC->LineTo(sDE);

	// Draw dimension arc using Arc() for exact consistency with original arc drawing
	CRect rcEllipse(sc.x - sr, sc.y - sr, sc.x + sr, sc.y + sr);
	pDC->Arc(rcEllipse, sDS, sDE);

	// Draw arrowheads at arc endpoints (tangent to arc)
	int arrowLen = max(8, (int)(8 * scale));
	auto drawArcArrow = [&](CPoint screenPt, double angle, bool atStart) {
		double sweepDir = m_dSweep > 0 ? 1.0 : -1.0;
		double tangentAng;
		if (atStart)
			tangentAng = angle + M_PI/2.0 * sweepDir - M_PI;
		else
			tangentAng = angle + M_PI/2.0 * sweepDir;
		double a1 = tangentAng + M_PI * 3.0 / 8.0;
		double a2 = tangentAng - M_PI * 3.0 / 8.0;
		CPoint p1((int)floor(screenPt.x + cos(a1) * arrowLen + 0.5), (int)floor(screenPt.y + sin(a1) * arrowLen + 0.5));
		CPoint p2((int)floor(screenPt.x + cos(a2) * arrowLen + 0.5), (int)floor(screenPt.y + sin(a2) * arrowLen + 0.5));
		pDC->MoveTo(screenPt); pDC->LineTo(p1);
		pDC->MoveTo(screenPt); pDC->LineTo(p2);
	};
	drawArcArrow(sDS, m_dStartAngle, true);
	drawArcArrow(sDE, m_dEndAngle, false);

	// Draw text
	double arcLen = (double)m_nArcRadius * fabs(m_dSweep);
	CString txt; txt.Format(L"%.2f", arcLen);
	CPoint txtPt;
	if (!(m_ptText.x == 0 && m_ptText.y == 0)) {
		txtPt = ToScreen(m_ptText, scale, offset);
	} else {
		double midAng = m_dStartAngle + m_dSweep * 0.5;
		double textR = m_nDimRadius + max(10, 10 * scale) / scale;
		txtPt = CPoint((int)floor(sc.x + textR * scale * cos(midAng) + 0.5), (int)floor(sc.y + textR * scale * sin(midAng) + 0.5));
	}
	CFont font; int h = max(8, (int)(12*scale)); font.CreatePointFont(h*10, _T("Consolas"));
	CFont* oldF = pDC->SelectObject(&font);
	COLORREF oldC = pDC->SetTextColor(m_color);
	int oldBk = pDC->SetBkMode(TRANSPARENT);
	if (m_bTextPlaced) pDC->TextOutW(txtPt.x+4, txtPt.y+4, txt);
	pDC->SetBkMode(oldBk); pDC->SetTextColor(oldC); pDC->SelectObject(oldF); font.DeleteObject();
	pDC->SelectObject(pOld);
}

bool CDimArcLengthEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
	double d = Distance(pt, ToScreen(m_ptCenter, scale, offset));
	int sr = (int)(m_nDimRadius * scale);
	return abs(d - sr) <= 8.0;
}

void CDimArcLengthEntity::Move(double dx, double dy) { m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy; m_ptText.x += (int)dx; m_ptText.y += (int)dy; }

CRect CDimArcLengthEntity::GetBounds() { CRect rc(m_ptCenter.x - m_nDimRadius - 30, m_ptCenter.y - m_nDimRadius - 30, m_ptCenter.x + m_nDimRadius + 30, m_ptCenter.y + m_nDimRadius + 30); return rc; }

CEntity* CDimArcLengthEntity::Clone() const {
	CDimArcLengthEntity* p = new CDimArcLengthEntity(m_ptCenter, m_nArcRadius, m_nDimRadius, m_dStartAngle, m_dEndAngle, m_dSweep);
	p->m_ptText = m_ptText; p->m_bTextPlaced = m_bTextPlaced;
	p->m_color = m_color; p->m_nLineStyle = m_nLineStyle; p->m_nLineWidth = m_nLineWidth;
	return p;
}

void CDimArcLengthEntity::Rotate(CPoint base, double angle) { m_ptCenter = RotatePoint(m_ptCenter, base, angle); m_ptText = RotatePoint(m_ptText, base, angle); m_dStartAngle += angle; m_dEndAngle += angle; }

void CDimArcLengthEntity::Scale(CPoint base, double factor) { m_ptCenter = ScalePoint(m_ptCenter, base, factor); m_nArcRadius = (int)(m_nArcRadius * factor + 0.5); m_nDimRadius = (int)(m_nDimRadius * factor + 0.5); m_ptText = ScalePoint(m_ptText, base, factor); }

void CDimArcLengthEntity::Mirror(CPoint p1, CPoint p2) { m_ptCenter = MirrorPoint(m_ptCenter, p1, p2); m_ptText = MirrorPoint(m_ptText, p1, p2); m_dStartAngle = M_PI - m_dStartAngle; m_dEndAngle = M_PI - m_dEndAngle; m_dSweep = -m_dSweep; }

void CDimArcLengthEntity::Serialize(CArchive& ar) {
	CEntity::Serialize(ar);
	if (ar.IsStoring()) ar << m_ptCenter.x << m_ptCenter.y << m_nArcRadius << m_nDimRadius << m_dStartAngle << m_dEndAngle << m_dSweep << m_ptText.x << m_ptText.y << m_bTextPlaced;
	else { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nArcRadius >> m_nDimRadius >> m_dStartAngle >> m_dEndAngle >> m_dSweep >> m_ptText.x >> m_ptText.y >> m_bTextPlaced; }
}
