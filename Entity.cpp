

#include "pch.h"
#include "Entity.h"
#include <cmath>
#include <algorithm>

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CEntity, CObject, 1)

int CEntity::m_nNextID = 1;

CEntity::CEntity()
    : m_nID(m_nNextID++)
    , m_Type(ENT_LINE)
    , m_color(RGB(0, 255, 209))
    , m_nLineStyle(PS_SOLID)
    , m_nLineWidth(1)
    , m_strLayer(_T("0"))
    , m_bSelected(false)
    , m_bVisible(true)
    , m_bUseLayerColor(false)
{}

CEntity::~CEntity() {}


void CEntity::Draw(CDC*, double, CPoint) {}
bool CEntity::HitTest(CPoint, double, CPoint) { return false; }
void CEntity::Move(double, double) {}
void CEntity::Rotate(CPoint, double) {}
void CEntity::Scale(CPoint, double) {}
void CEntity::Mirror(CPoint, CPoint) {}
CRect CEntity::GetBounds() { return CRect(0,0,0,0); }
CEntity* CEntity::Clone() const { return nullptr; }
int CEntity::GetGripCount() { return 0; }
CPoint CEntity::GetGrip(int) { return CPoint(0,0); }
void CEntity::SetGrip(int, CPoint) {}
void CEntity::GetSnapPoints(std::vector<CPoint>&, std::vector<SnapType>&) const {}

void CEntity::Serialize(CArchive& ar)
{
    CObject::Serialize(ar);
    if (ar.IsStoring()) {
        ar << m_nID << (int)m_Type << m_color << m_nLineStyle << m_nLineWidth
           << m_strLayer << m_bVisible;
    } else {
        int nType = 0;
        ar >> m_nID >> nType >> m_color >> m_nLineStyle >> m_nLineWidth
           >> m_strLayer >> m_bVisible;
        m_Type = (EntityType)nType;
        m_bSelected = false;
        if (m_nID >= m_nNextID) m_nNextID = m_nID + 1;
    }
}

CPoint CEntity::ToWorld(CPoint screen, double scale, CPoint offset) const
{
    return CPoint((int)((screen.x - offset.x) / scale),
                  (int)((screen.y - offset.y) / scale));
}

CPoint CEntity::ToScreen(CPoint world, double scale, CPoint offset) const
{
    return CPoint((int)(world.x * scale + offset.x),
                  (int)(world.y * scale + offset.y));
}

CRect CEntity::ToScreenRect(CRect world, double scale, CPoint offset) const
{
    return CRect((int)(world.left * scale + offset.x),
                 (int)(world.top * scale + offset.y),
                 (int)(world.right * scale + offset.x),
                 (int)(world.bottom * scale + offset.y));
}

int CEntity::HitTestGrip(CPoint pt, double scale, CPoint offset)
{
    int nGrips = GetGripCount();
    for (int i = 0; i < nGrips; ++i) {
        CPoint grip = GetGrip(i);
        CPoint screenGrip = ToScreen(grip, scale, offset);
        if (abs(pt.x - screenGrip.x) <= 6 && abs(pt.y - screenGrip.y) <= 6)
            return i;
    }
    return -1;
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CLineEntity, CEntity, 1)

CLineEntity::CLineEntity() : m_ptStart(0,0), m_ptEnd(0,0) { m_Type = ENT_LINE; }
CLineEntity::CLineEntity(CPoint start, CPoint end) : m_ptStart(start), m_ptEnd(end) { m_Type = ENT_LINE; }

void CLineEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;

    CPoint p1 = ToScreen(m_ptStart, scale, offset);
    CPoint p2 = ToScreen(m_ptEnd, scale, offset);

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);
    pDC->MoveTo(p1);
    pDC->LineTo(p2);


    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        CRect rc = GetBounds();
        CRect rcScreen = ToScreenRect(rc, scale, offset);
        rcScreen.InflateRect(4, 4);
        pDC->DrawFocusRect(rcScreen);


        CBrush gripBrush(RGB(0, 128, 255));
        CBrush* pOldBrush = pDC->SelectObject(&gripBrush);
        for (int i = 0; i < GetGripCount(); ++i) {
            CPoint g = ToScreen(GetGrip(i), scale, offset);
            pDC->Rectangle(g.x - 4, g.y - 4, g.x + 5, g.y + 5);
        }
        pDC->SelectObject(pOldBrush);
    }
    pDC->SelectObject(pOldPen);
}

bool CLineEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint p1 = ToScreen(m_ptStart, scale, offset);
    CPoint p2 = ToScreen(m_ptEnd, scale, offset);
    double dist = PointToLineDistance(pt, p1, p2);
    return dist <= 5.0;
}

void CLineEntity::Move(double dx, double dy) {
    m_ptStart.x += (int)dx; m_ptStart.y += (int)dy;
    m_ptEnd.x += (int)dx;   m_ptEnd.y += (int)dy;
}

CRect CLineEntity::GetBounds() {
    CRect rc(m_ptStart, m_ptEnd);
    rc.NormalizeRect();
    rc.InflateRect(2, 2);
    return rc;
}

CEntity* CLineEntity::Clone() const {
    CLineEntity* p = new CLineEntity(m_ptStart, m_ptEnd);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CLineEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptStart.x << m_ptStart.y << m_ptEnd.x << m_ptEnd.y; }
    else                 { ar >> m_ptStart.x >> m_ptStart.y >> m_ptEnd.x >> m_ptEnd.y; }
}

CPoint CLineEntity::GetGrip(int index) {
    return index == 0 ? m_ptStart : m_ptEnd;
}

void CLineEntity::SetGrip(int index, CPoint pt) {
    if (index == 0) m_ptStart = pt; else m_ptEnd = pt;
}

void CLineEntity::Rotate(CPoint base, double angle) {
    m_ptStart = RotatePoint(m_ptStart, base, angle);
    m_ptEnd = RotatePoint(m_ptEnd, base, angle);
}

void CLineEntity::Scale(CPoint base, double factor) {
    m_ptStart = ScalePoint(m_ptStart, base, factor);
    m_ptEnd = ScalePoint(m_ptEnd, base, factor);
}

void CLineEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptStart = MirrorPoint(m_ptStart, p1, p2);
    m_ptEnd = MirrorPoint(m_ptEnd, p1, p2);
}

void CLineEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptStart); types.push_back(SNAP_ENDPOINT);
    points.push_back(m_ptEnd);   types.push_back(SNAP_ENDPOINT);
    points.push_back(MidPoint(m_ptStart, m_ptEnd)); types.push_back(SNAP_MIDPOINT);
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CCircleEntity, CEntity, 1)

CCircleEntity::CCircleEntity() : m_ptCenter(0,0), m_nRadius(50) { m_Type = ENT_CIRCLE; }
CCircleEntity::CCircleEntity(CPoint center, int radius) : m_ptCenter(center), m_nRadius(radius) { m_Type = ENT_CIRCLE; }

void CCircleEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int r = (int)(m_nRadius * scale);
    if (r < 1) r = 1;

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
    CPen* pOldPen = pDC->SelectObject(&pen);
    pDC->Ellipse(c.x - r, c.y - r, c.x + r, c.y + r);

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        CRect rc = GetBounds();
        CRect rcScreen = ToScreenRect(rc, scale, offset);
        rcScreen.InflateRect(4, 4);
        pDC->DrawFocusRect(rcScreen);

        CBrush gripBrush(RGB(0, 128, 255));
        pDC->SelectObject(&gripBrush);
        for (int i = 0; i < GetGripCount(); ++i) {
            CPoint g = ToScreen(GetGrip(i), scale, offset);
            pDC->Rectangle(g.x - 4, g.y - 4, g.x + 5, g.y + 5);
        }
        pDC->SelectObject(pOldBrush);
    }
    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

bool CCircleEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int r = (int)(m_nRadius * scale);
    double dist = Distance(pt, c);
    return fabs(dist - r) <= 5.0;
}

void CCircleEntity::Move(double dx, double dy)
{
    m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy;
}

CRect CCircleEntity::GetBounds() {
    return CRect(m_ptCenter.x - m_nRadius, m_ptCenter.y - m_nRadius,
                 m_ptCenter.x + m_nRadius, m_ptCenter.y + m_nRadius);
}

CEntity* CCircleEntity::Clone() const {
    CCircleEntity* p = new CCircleEntity(m_ptCenter, m_nRadius);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CCircleEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptCenter.x << m_ptCenter.y << m_nRadius; }
    else                 { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nRadius; }
}

CPoint CCircleEntity::GetGrip(int index) {
    switch (index) {
        case 0: return m_ptCenter;
        case 1: return CPoint(m_ptCenter.x + m_nRadius, m_ptCenter.y);
        case 2: return CPoint(m_ptCenter.x - m_nRadius, m_ptCenter.y);
        case 3: return CPoint(m_ptCenter.x, m_ptCenter.y + m_nRadius);
        case 4: return CPoint(m_ptCenter.x, m_ptCenter.y - m_nRadius);
        default: return m_ptCenter;
    }
}

void CCircleEntity::SetGrip(int index, CPoint pt) {
    if (index == 0) { m_ptCenter = pt; }
    else { m_nRadius = (int)Distance(m_ptCenter, pt); }
}

void CCircleEntity::Rotate(CPoint base, double angle) {
    m_ptCenter = RotatePoint(m_ptCenter, base, angle);
}

void CCircleEntity::Scale(CPoint base, double factor) {
    m_ptCenter = ScalePoint(m_ptCenter, base, factor);
    m_nRadius = (int)(m_nRadius * fabs(factor) + 0.5);
    if (m_nRadius < 1) m_nRadius = 1;
}

void CCircleEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptCenter = MirrorPoint(m_ptCenter, p1, p2);
}

void CCircleEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptCenter); types.push_back(SNAP_CENTER);
    points.push_back(CPoint(m_ptCenter.x + m_nRadius, m_ptCenter.y)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x - m_nRadius, m_ptCenter.y)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x, m_ptCenter.y + m_nRadius)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x, m_ptCenter.y - m_nRadius)); types.push_back(SNAP_QUADRANT);
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CArcEntity, CEntity, 1)

CArcEntity::CArcEntity() : m_ptStart(0,0), m_ptMid(0,0), m_ptEnd(0,0),
    m_ptCenter(0,0), m_nRadius(0) { m_Type = ENT_ARC; }
CArcEntity::CArcEntity(CPoint start, CPoint mid, CPoint end)
    : m_ptStart(start), m_ptMid(mid), m_ptEnd(end) { m_Type = ENT_ARC; CalcGeometry(); }

void CArcEntity::SetArcByCenter(CPoint center, int radius, double angStart, double angEnd)
{
    m_ptCenter = center;
    m_nRadius = radius;
    int r = radius;
    m_ptStart = CPoint((int)(center.x + r * cos(angStart)),
                       (int)(center.y + r * sin(angStart)));
    m_ptEnd   = CPoint((int)(center.x + r * cos(angEnd)),
                       (int)(center.y + r * sin(angEnd)));
    // Mid point at halfway angle
    double sweep = angEnd - angStart;
    if (sweep < 0) sweep += 2.0 * M_PI;
    double angMid = angStart + sweep / 2.0;
    m_ptMid = CPoint((int)(center.x + r * cos(angMid)),
                     (int)(center.y + r * sin(angMid)));
}

void CArcEntity::CalcGeometry()
{
    double x1 = m_ptStart.x, y1 = m_ptStart.y;
    double x2 = m_ptMid.x,   y2 = m_ptMid.y;
    double x3 = m_ptEnd.x,   y3 = m_ptEnd.y;

    double d = 2.0 * (x1*(y2-y3) + x2*(y3-y1) + x3*(y1-y2));
    if (fabs(d) < 1e-6) { m_ptCenter = MidPoint(m_ptStart, m_ptEnd); m_nRadius = 1; return; }

    double cx = ((x1*x1 + y1*y1)*(y2-y3) + (x2*x2 + y2*y2)*(y3-y1) + (x3*x3 + y3*y3)*(y1-y2)) / d;
    double cy = ((x1*x1 + y1*y1)*(x3-x2) + (x2*x2 + y2*y2)*(x1-x3) + (x3*x3 + y3*y3)*(x2-x1)) / d;
    m_ptCenter = CPoint((int)cx, (int)cy);
    m_nRadius = (int)Distance(m_ptCenter, m_ptStart);
}

void CArcEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int r = (int)(m_nRadius * scale);
    if (r < 2) r = 2;


    double angStart = atan2((double)(m_ptStart.y - m_ptCenter.y), (double)(m_ptStart.x - m_ptCenter.x));
    double angEnd   = atan2((double)(m_ptEnd.y - m_ptCenter.y),   (double)(m_ptEnd.x - m_ptCenter.x));
    double angMid   = atan2((double)(m_ptMid.y - m_ptCenter.y),   (double)(m_ptMid.x - m_ptCenter.x));


    double sweep = angEnd - angStart;
    if (sweep < 0) sweep += 2 * M_PI;
    double midSweep = angMid - angStart;
    if (midSweep < 0) midSweep += 2 * M_PI;
    if (midSweep > sweep) {
        double tmp = angStart; angStart = angEnd; angEnd = tmp;
        sweep = 2 * M_PI - sweep;
    }

    CRect rcEllipse(c.x - r, c.y - r, c.x + r, c.y + r);

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);


    CPoint ptStartArc((int)(c.x + r * cos(angStart)), (int)(c.y + r * sin(angStart)));
    CPoint ptEndArc((int)(c.x + r * cos(angEnd)), (int)(c.y + r * sin(angEnd)));
    pDC->Arc(rcEllipse, ptStartArc, ptEndArc);

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        pDC->SelectStockObject(NULL_BRUSH);
        pDC->Arc(rcEllipse, ptStartArc, ptEndArc);
    }
    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

bool CArcEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int r = (int)(m_nRadius * scale);
    double dist = Distance(pt, c);
    return fabs(dist - r) <= 6.0;
}

void CArcEntity::Move(double dx, double dy)
{
    m_ptStart.x += (int)dx; m_ptStart.y += (int)dy;
    m_ptMid.x   += (int)dx; m_ptMid.y   += (int)dy;
    m_ptEnd.x   += (int)dx; m_ptEnd.y   += (int)dy;
    m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy;
}

CRect CArcEntity::GetBounds() {
    CRect rc(m_ptCenter.x - m_nRadius, m_ptCenter.y - m_nRadius,
             m_ptCenter.x + m_nRadius, m_ptCenter.y + m_nRadius);
    return rc;
}

CEntity* CArcEntity::Clone() const {
    CArcEntity* p = new CArcEntity(m_ptStart, m_ptMid, m_ptEnd);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CArcEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) {
        ar << m_ptStart.x << m_ptStart.y << m_ptMid.x << m_ptMid.y
           << m_ptEnd.x << m_ptEnd.y;
    } else {
        ar >> m_ptStart.x >> m_ptStart.y >> m_ptMid.x >> m_ptMid.y
           >> m_ptEnd.x >> m_ptEnd.y;
        CalcGeometry();
    }
}

CPoint CArcEntity::GetGrip(int index) {
    switch (index) {
        case 0: return m_ptStart;
        case 1: return m_ptMid;
        case 2: return m_ptEnd;
        default: return m_ptStart;
    }
}

void CArcEntity::SetGrip(int index, CPoint pt) {
    switch (index) {
        case 0: m_ptStart = pt; break;
        case 1: m_ptMid   = pt; break;
        case 2: m_ptEnd   = pt; break;
    }
    CalcGeometry();
}

void CArcEntity::Rotate(CPoint base, double angle) {
    m_ptStart = RotatePoint(m_ptStart, base, angle);
    m_ptMid = RotatePoint(m_ptMid, base, angle);
    m_ptEnd = RotatePoint(m_ptEnd, base, angle);
    CalcGeometry();
}

void CArcEntity::Scale(CPoint base, double factor) {
    m_ptStart = ScalePoint(m_ptStart, base, factor);
    m_ptMid = ScalePoint(m_ptMid, base, factor);
    m_ptEnd = ScalePoint(m_ptEnd, base, factor);
    CalcGeometry();
}

void CArcEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptStart = MirrorPoint(m_ptStart, p1, p2);
    m_ptMid = MirrorPoint(m_ptMid, p1, p2);
    m_ptEnd = MirrorPoint(m_ptEnd, p1, p2);
    CalcGeometry();
}

void CArcEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptStart);  types.push_back(SNAP_ENDPOINT);
    points.push_back(m_ptEnd);    types.push_back(SNAP_ENDPOINT);
    points.push_back(m_ptMid);    types.push_back(SNAP_MIDPOINT);
    points.push_back(m_ptCenter); types.push_back(SNAP_CENTER);
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CRectangleEntity, CEntity, 1)

CRectangleEntity::CRectangleEntity() : m_ptCorner1(0,0), m_ptCorner2(0,0) { m_Type = ENT_RECTANGLE; }
CRectangleEntity::CRectangleEntity(CPoint c1, CPoint c2) : m_ptCorner1(c1), m_ptCorner2(c2) { m_Type = ENT_RECTANGLE; }

void CRectangleEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    CPoint p1 = ToScreen(m_ptCorner1, scale, offset);
    CPoint p2 = ToScreen(m_ptCorner2, scale, offset);

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);

    int l = min(p1.x, p2.x), r = max(p1.x, p2.x);
    int t = min(p1.y, p2.y), b = max(p1.y, p2.y);
    pDC->Rectangle(l, t, r, b);

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        pDC->Rectangle(l - 2, t - 2, r + 2, b + 2);
    }
    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

bool CRectangleEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint p1 = ToScreen(m_ptCorner1, scale, offset);
    CPoint p2 = ToScreen(m_ptCorner2, scale, offset);
    CRect rc(p1, p2); rc.NormalizeRect();
    rc.InflateRect(3, 3);
    return rc.PtInRect(pt);
}

void CRectangleEntity::Move(double dx, double dy)
{
    m_ptCorner1.x += (int)dx; m_ptCorner1.y += (int)dy;
    m_ptCorner2.x += (int)dx; m_ptCorner2.y += (int)dy;
}

CRect CRectangleEntity::GetBounds() {
    CRect rc(m_ptCorner1, m_ptCorner2); rc.NormalizeRect(); return rc;
}

CEntity* CRectangleEntity::Clone() const {
    CRectangleEntity* p = new CRectangleEntity(m_ptCorner1, m_ptCorner2);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CRectangleEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptCorner1.x << m_ptCorner1.y << m_ptCorner2.x << m_ptCorner2.y; }
    else                 { ar >> m_ptCorner1.x >> m_ptCorner1.y >> m_ptCorner2.x >> m_ptCorner2.y; }
}

CPoint CRectangleEntity::GetGrip(int index) {
    CRect rc(m_ptCorner1, m_ptCorner2); rc.NormalizeRect();
    switch (index) {
        case 0: return rc.TopLeft();
        case 1: return CPoint(rc.right, rc.top);
        case 2: return rc.BottomRight();
        case 3: return CPoint(rc.left, rc.bottom);
        default: return rc.TopLeft();
    }
}

void CRectangleEntity::SetGrip(int index, CPoint pt) {
    CRect rc(m_ptCorner1, m_ptCorner2); rc.NormalizeRect();
    switch (index) {
        case 0: m_ptCorner1 = pt; m_ptCorner2 = rc.BottomRight(); break;
        case 1: m_ptCorner1 = CPoint(rc.left, pt.y); m_ptCorner2 = CPoint(pt.x, rc.bottom); break;
        case 2: m_ptCorner1 = rc.TopLeft(); m_ptCorner2 = pt; break;
        case 3: m_ptCorner1 = CPoint(pt.x, rc.top); m_ptCorner2 = CPoint(rc.right, pt.y); break;
    }
}

void CRectangleEntity::Rotate(CPoint base, double angle) {
    m_ptCorner1 = RotatePoint(m_ptCorner1, base, angle);
    m_ptCorner2 = RotatePoint(m_ptCorner2, base, angle);
}

void CRectangleEntity::Scale(CPoint base, double factor) {
    m_ptCorner1 = ScalePoint(m_ptCorner1, base, factor);
    m_ptCorner2 = ScalePoint(m_ptCorner2, base, factor);
}

void CRectangleEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptCorner1 = MirrorPoint(m_ptCorner1, p1, p2);
    m_ptCorner2 = MirrorPoint(m_ptCorner2, p1, p2);
}

void CRectangleEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    CRect rc(m_ptCorner1, m_ptCorner2); rc.NormalizeRect();
    points.push_back(rc.TopLeft());          types.push_back(SNAP_ENDPOINT);
    points.push_back(CPoint(rc.right, rc.top)); types.push_back(SNAP_ENDPOINT);
    points.push_back(rc.BottomRight());      types.push_back(SNAP_ENDPOINT);
    points.push_back(CPoint(rc.left, rc.bottom)); types.push_back(SNAP_ENDPOINT);
    points.push_back(MidPoint(rc.TopLeft(), CPoint(rc.right, rc.top)));     types.push_back(SNAP_MIDPOINT);
    points.push_back(MidPoint(CPoint(rc.right, rc.top), rc.BottomRight())); types.push_back(SNAP_MIDPOINT);
    points.push_back(MidPoint(rc.BottomRight(), CPoint(rc.left, rc.bottom))); types.push_back(SNAP_MIDPOINT);
    points.push_back(MidPoint(CPoint(rc.left, rc.bottom), rc.TopLeft()));   types.push_back(SNAP_MIDPOINT);
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CPolygonEntity, CEntity, 1)

CPolygonEntity::CPolygonEntity() : m_ptCenter(0,0), m_nRadius(50), m_nSides(6) { m_Type = ENT_POLYGON; }
CPolygonEntity::CPolygonEntity(CPoint center, int radius, int sides)
    : m_ptCenter(center), m_nRadius(radius), m_nSides(sides) { m_Type = ENT_POLYGON; }

void CPolygonEntity::GetVertices(std::vector<CPoint>& vertices) const
{
    vertices.clear();
    double angleStep = 2.0 * M_PI / m_nSides;
    double startAngle = -M_PI / 2.0;
    for (int i = 0; i < m_nSides; ++i) {
        double a = startAngle + angleStep * i;
        vertices.push_back(CPoint(
            (int)(m_ptCenter.x + m_nRadius * cos(a)),
            (int)(m_ptCenter.y + m_nRadius * sin(a))
        ));
    }
}

void CPolygonEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    std::vector<CPoint> verts;
    GetVertices(verts);

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);

    for (int i = 0; i < m_nSides; ++i) {
        CPoint p1 = ToScreen(verts[i], scale, offset);
        CPoint p2 = ToScreen(verts[(i+1) % m_nSides], scale, offset);
        pDC->MoveTo(p1);
        pDC->LineTo(p2);
    }

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        CRect rc = ToScreenRect(GetBounds(), scale, offset);
        rc.InflateRect(4, 4);
        pDC->DrawFocusRect(rc);
    }
    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

bool CPolygonEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    std::vector<CPoint> verts;
    GetVertices(verts);
    for (int i = 0; i < m_nSides; ++i) {
        CPoint p1 = ToScreen(verts[i], scale, offset);
        CPoint p2 = ToScreen(verts[(i+1) % m_nSides], scale, offset);
        if (PointToLineDistance(pt, p1, p2) <= 5.0) return true;
    }
    return false;
}

void CPolygonEntity::Move(double dx, double dy) {
    m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy;
}

CRect CPolygonEntity::GetBounds() {
    return CRect(m_ptCenter.x - m_nRadius, m_ptCenter.y - m_nRadius,
                 m_ptCenter.x + m_nRadius, m_ptCenter.y + m_nRadius);
}

CEntity* CPolygonEntity::Clone() const {
    CPolygonEntity* p = new CPolygonEntity(m_ptCenter, m_nRadius, m_nSides);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CPolygonEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptCenter.x << m_ptCenter.y << m_nRadius << m_nSides; }
    else                 { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nRadius >> m_nSides; }
}

CPoint CPolygonEntity::GetGrip(int index) {
    if (index == 0) return m_ptCenter;
    std::vector<CPoint> verts;
    GetVertices(verts);
    return verts[index - 1];
}

void CPolygonEntity::SetGrip(int index, CPoint pt) {
    if (index == 0) { m_ptCenter = pt; }
    else { m_nRadius = (int)Distance(m_ptCenter, pt); }
}

void CPolygonEntity::Rotate(CPoint base, double angle) {
    m_ptCenter = RotatePoint(m_ptCenter, base, angle);
}

void CPolygonEntity::Scale(CPoint base, double factor) {
    m_ptCenter = ScalePoint(m_ptCenter, base, factor);
    m_nRadius = (int)(m_nRadius * fabs(factor) + 0.5);
    if (m_nRadius < 1) m_nRadius = 1;
}

void CPolygonEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptCenter = MirrorPoint(m_ptCenter, p1, p2);
}

void CPolygonEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptCenter); types.push_back(SNAP_CENTER);
    std::vector<CPoint> verts;
    GetVertices(verts);
    for (size_t i = 0; i < verts.size(); ++i) {
        points.push_back(verts[i]); types.push_back(SNAP_ENDPOINT);
        points.push_back(MidPoint(verts[i], verts[(i+1) % verts.size()])); types.push_back(SNAP_MIDPOINT);
    }
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CEllipseEntity, CEntity, 1)

CEllipseEntity::CEllipseEntity() : m_ptCenter(0,0), m_nRadiusX(80), m_nRadiusY(40) { m_Type = ENT_ELLIPSE; }
CEllipseEntity::CEllipseEntity(CPoint center, int rx, int ry)
    : m_ptCenter(center), m_nRadiusX(rx), m_nRadiusY(ry) { m_Type = ENT_ELLIPSE; }

void CEllipseEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int rx = (int)(m_nRadiusX * scale);
    int ry = (int)(m_nRadiusY * scale);
    if (rx < 1) rx = 1; if (ry < 1) ry = 1;

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);
    CBrush* pOldBrush = (CBrush*)pDC->SelectStockObject(NULL_BRUSH);
    pDC->Ellipse(c.x - rx, c.y - ry, c.x + rx, c.y + ry);

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        pDC->Ellipse(c.x - rx - 2, c.y - ry - 2, c.x + rx + 2, c.y + ry + 2);
    }
    pDC->SelectObject(pOldPen);
    pDC->SelectObject(pOldBrush);
}

bool CEllipseEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint c = ToScreen(m_ptCenter, scale, offset);
    int rx = (int)(m_nRadiusX * scale);
    int ry = (int)(m_nRadiusY * scale);
    if (rx == 0 || ry == 0) return false;
    double dx = (pt.x - c.x) / (double)rx;
    double dy = (pt.y - c.y) / (double)ry;
    double val = sqrt(dx*dx + dy*dy);
    return fabs(val - 1.0) <= 0.08;
}

void CEllipseEntity::Move(double dx, double dy) {
    m_ptCenter.x += (int)dx; m_ptCenter.y += (int)dy;
}

CRect CEllipseEntity::GetBounds() {
    return CRect(m_ptCenter.x - m_nRadiusX, m_ptCenter.y - m_nRadiusY,
                 m_ptCenter.x + m_nRadiusX, m_ptCenter.y + m_nRadiusY);
}

CEntity* CEllipseEntity::Clone() const {
    CEllipseEntity* p = new CEllipseEntity(m_ptCenter, m_nRadiusX, m_nRadiusY);
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CEllipseEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptCenter.x << m_ptCenter.y << m_nRadiusX << m_nRadiusY; }
    else                 { ar >> m_ptCenter.x >> m_ptCenter.y >> m_nRadiusX >> m_nRadiusY; }
}

CPoint CEllipseEntity::GetGrip(int index) {
    switch (index) {
        case 0: return m_ptCenter;
        case 1: return CPoint(m_ptCenter.x + m_nRadiusX, m_ptCenter.y);
        case 2: return CPoint(m_ptCenter.x - m_nRadiusX, m_ptCenter.y);
        case 3: return CPoint(m_ptCenter.x, m_ptCenter.y + m_nRadiusY);
        case 4: return CPoint(m_ptCenter.x, m_ptCenter.y - m_nRadiusY);
        default: return m_ptCenter;
    }
}

void CEllipseEntity::SetGrip(int index, CPoint pt) {
    if (index == 0) { m_ptCenter = pt; }
    else if (index == 1) { m_nRadiusX = abs(pt.x - m_ptCenter.x); }
    else if (index == 2) { m_nRadiusX = abs(pt.x - m_ptCenter.x); }
    else if (index == 3) { m_nRadiusY = abs(pt.y - m_ptCenter.y); }
    else if (index == 4) { m_nRadiusY = abs(pt.y - m_ptCenter.y); }
}

void CEllipseEntity::Rotate(CPoint base, double angle) {
    m_ptCenter = RotatePoint(m_ptCenter, base, angle);
}

void CEllipseEntity::Scale(CPoint base, double factor) {
    m_ptCenter = ScalePoint(m_ptCenter, base, factor);
    m_nRadiusX = (int)(m_nRadiusX * fabs(factor) + 0.5);
    m_nRadiusY = (int)(m_nRadiusY * fabs(factor) + 0.5);
    if (m_nRadiusX < 1) m_nRadiusX = 1;
    if (m_nRadiusY < 1) m_nRadiusY = 1;
}

void CEllipseEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptCenter = MirrorPoint(m_ptCenter, p1, p2);
}

void CEllipseEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptCenter); types.push_back(SNAP_CENTER);
    points.push_back(CPoint(m_ptCenter.x + m_nRadiusX, m_ptCenter.y)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x - m_nRadiusX, m_ptCenter.y)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x, m_ptCenter.y + m_nRadiusY)); types.push_back(SNAP_QUADRANT);
    points.push_back(CPoint(m_ptCenter.x, m_ptCenter.y - m_nRadiusY)); types.push_back(SNAP_QUADRANT);
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CPolylineEntity, CEntity, 1)

CPolylineEntity::CPolylineEntity() : m_bClosed(false) { m_Type = ENT_POLYLINE; }
CPolylineEntity::CPolylineEntity(const std::vector<CPoint>& points)
    : m_vertices(points), m_bClosed(false) { m_Type = ENT_POLYLINE; }

void CPolylineEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible || m_vertices.size() < 2) return;

    CPen pen(m_nLineStyle, m_nLineWidth, m_color);
    CPen* pOldPen = pDC->SelectObject(&pen);

    int n = (int)m_vertices.size();
    pDC->MoveTo(ToScreen(m_vertices[0], scale, offset));
    for (int i = 1; i < n; ++i)
        pDC->LineTo(ToScreen(m_vertices[i], scale, offset));


    if (m_bClosed && n > 2)
        pDC->LineTo(ToScreen(m_vertices[0], scale, offset));

    if (m_bSelected) {
        CPen selPen(PS_DASH, 1, RGB(0, 255, 255));
        pDC->SelectObject(&selPen);
        CRect rc = ToScreenRect(GetBounds(), scale, offset);
        rc.InflateRect(4, 4);
        pDC->DrawFocusRect(rc);

        CBrush gripBrush(RGB(0, 128, 255));
        CBrush* pOldBrush = pDC->SelectObject(&gripBrush);
        for (int i = 0; i < (int)m_vertices.size(); ++i) {
            CPoint g = ToScreen(m_vertices[i], scale, offset);
            pDC->Rectangle(g.x - 4, g.y - 4, g.x + 5, g.y + 5);
        }
        pDC->SelectObject(pOldBrush);
    }
    pDC->SelectObject(pOldPen);
}

bool CPolylineEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    int n = (int)m_vertices.size();
    for (int i = 0; i < n - 1; ++i) {
        CPoint p1 = ToScreen(m_vertices[i], scale, offset);
        CPoint p2 = ToScreen(m_vertices[i+1], scale, offset);
        if (PointToLineDistance(pt, p1, p2) <= 5.0) return true;
    }
    if (m_bClosed && n > 2) {
        CPoint p1 = ToScreen(m_vertices.back(), scale, offset);
        CPoint p2 = ToScreen(m_vertices[0], scale, offset);
        if (PointToLineDistance(pt, p1, p2) <= 5.0) return true;
    }
    return false;
}

void CPolylineEntity::Move(double dx, double dy) {
    for (auto& v : m_vertices) { v.x += (int)dx; v.y += (int)dy; }
}

CRect CPolylineEntity::GetBounds() {
    if (m_vertices.empty()) return CRect(0,0,0,0);
    CRect rc(m_vertices[0], m_vertices[0]);
    for (const auto& v : m_vertices) {
        rc.left   = min(rc.left, v.x);
        rc.right  = max(rc.right, v.x);
        rc.top    = min(rc.top, v.y);
        rc.bottom = max(rc.bottom, v.y);
    }
    rc.InflateRect(2, 2);
    return rc;
}

CEntity* CPolylineEntity::Clone() const {
    CPolylineEntity* p = new CPolylineEntity(m_vertices);
    p->m_bClosed = m_bClosed;
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CPolylineEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) {
        int n = (int)m_vertices.size();
        ar << n << m_bClosed;
        for (const auto& v : m_vertices) ar << v.x << v.y;
    } else {
        int n; ar >> n >> m_bClosed;
        m_vertices.resize(n);
        for (auto& v : m_vertices) ar >> v.x >> v.y;
    }
}

CPoint CPolylineEntity::GetGrip(int index) {
    return m_vertices[index];
}

void CPolylineEntity::SetGrip(int index, CPoint pt) {
    m_vertices[index] = pt;
}

void CPolylineEntity::Rotate(CPoint base, double angle) {
    for (auto& v : m_vertices) v = RotatePoint(v, base, angle);
}

void CPolylineEntity::Scale(CPoint base, double factor) {
    for (auto& v : m_vertices) v = ScalePoint(v, base, factor);
}

void CPolylineEntity::Mirror(CPoint p1, CPoint p2) {
    for (auto& v : m_vertices) v = MirrorPoint(v, p1, p2);
}

void CPolylineEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    int n = (int)m_vertices.size();
    for (int i = 0; i < n; ++i) {
        points.push_back(m_vertices[i]); types.push_back(SNAP_ENDPOINT);
    }
    for (int i = 0; i < n - 1; ++i) {
        points.push_back(MidPoint(m_vertices[i], m_vertices[i+1])); types.push_back(SNAP_MIDPOINT);
    }
    if (m_bClosed && n > 2) {
        points.push_back(MidPoint(m_vertices.back(), m_vertices[0])); types.push_back(SNAP_MIDPOINT);
    }
}

// ===========================================================

// ===========================================================
IMPLEMENT_SERIAL(CTextEntity, CEntity, 1)

CTextEntity::CTextEntity() : m_ptPosition(0,0), m_strText(_T("Text")), m_nHeight(20), m_dRotation(0) { m_Type = ENT_TEXT; }
CTextEntity::CTextEntity(CPoint pos, const CString& text, int height)
    : m_ptPosition(pos), m_strText(text), m_nHeight(height), m_dRotation(0) { m_Type = ENT_TEXT; }

void CTextEntity::Draw(CDC* pDC, double scale, CPoint offset)
{
    if (!m_bVisible) return;
    CPoint pos = ToScreen(m_ptPosition, scale, offset);
    int scaledHeight = (int)(m_nHeight * scale);
    if (scaledHeight < 5) scaledHeight = 5;

    int rotDeg = (int)(m_dRotation * 180.0 / M_PI * 10.0);
    CFont font;
    font.CreateFont(scaledHeight, 0, rotDeg, rotDeg, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, _T("Consolas"));
    CFont* pOldFont = pDC->SelectObject(&font);
    COLORREF oldColor = pDC->SetTextColor(m_color);
    int oldBkMode = pDC->SetBkMode(TRANSPARENT);

    pDC->TextOutW(pos.x, pos.y, m_strText);

    pDC->SetBkMode(oldBkMode);
    pDC->SetTextColor(oldColor);
    pDC->SelectObject(pOldFont);
    font.DeleteObject();

    if (m_bSelected) {
        CSize sz = pDC->GetTextExtent(m_strText);
        CPen selPen(PS_DOT, 1, RGB(0, 255, 255));
        CPen* oldPen = pDC->SelectObject(&selPen);
        pDC->MoveTo(pos.x, pos.y + scaledHeight + 2);
        pDC->LineTo(pos.x + sz.cx, pos.y + scaledHeight + 2);
        pDC->SelectObject(oldPen);
    }
}

bool CTextEntity::HitTest(CPoint pt, double scale, CPoint offset)
{
    CPoint pos = ToScreen(m_ptPosition, scale, offset);
    CClientDC dc(AfxGetMainWnd());
    CFont font;
    int h = (int)(m_nHeight * scale);
    font.CreateFont(h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    DEFAULT_QUALITY, DEFAULT_PITCH | FF_MODERN, _T("Consolas"));
    CFont* old = dc.SelectObject(&font);
    CSize sz = dc.GetTextExtent(m_strText);
    dc.SelectObject(old);
    font.DeleteObject();

    CRect rc(pos.x, pos.y, pos.x + sz.cx, pos.y + sz.cy);
    rc.InflateRect(3, 3);
    return rc.PtInRect(pt);
}

void CTextEntity::Move(double dx, double dy) {
    m_ptPosition.x += (int)dx; m_ptPosition.y += (int)dy;
}

CRect CTextEntity::GetBounds() {

    int w = m_strText.GetLength() * m_nHeight / 2;
    return CRect(m_ptPosition.x, m_ptPosition.y,
                 m_ptPosition.x + w, m_ptPosition.y + m_nHeight);
}

CEntity* CTextEntity::Clone() const {
    CTextEntity* p = new CTextEntity(m_ptPosition, m_strText, m_nHeight);
    p->m_dRotation = m_dRotation;
    p->m_color = m_color; p->m_nLineStyle = m_nLineStyle;
    p->m_nLineWidth = m_nLineWidth; p->m_strLayer = m_strLayer;
    return p;
}

void CTextEntity::Serialize(CArchive& ar) {
    CEntity::Serialize(ar);
    if (ar.IsStoring()) { ar << m_ptPosition.x << m_ptPosition.y << m_strText << m_nHeight << m_dRotation; }
    else                 { ar >> m_ptPosition.x >> m_ptPosition.y >> m_strText >> m_nHeight >> m_dRotation; }
}

CPoint CTextEntity::GetGrip(int index) {
    return index == 0 ? m_ptPosition : CPoint(m_ptPosition.x + 50, m_ptPosition.y + 30);
}

void CTextEntity::SetGrip(int index, CPoint pt) {
    if (index == 0) m_ptPosition = pt;
}

void CTextEntity::Rotate(CPoint base, double angle) {
    m_ptPosition = RotatePoint(m_ptPosition, base, angle);
    m_dRotation += angle;
}

void CTextEntity::Scale(CPoint base, double factor) {
    m_ptPosition = ScalePoint(m_ptPosition, base, factor);
    m_nHeight = (int)(m_nHeight * fabs(factor) + 0.5);
    if (m_nHeight < 2) m_nHeight = 2;
}

void CTextEntity::Mirror(CPoint p1, CPoint p2) {
    m_ptPosition = MirrorPoint(m_ptPosition, p1, p2);
}

void CTextEntity::GetSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const {
    points.push_back(m_ptPosition); types.push_back(SNAP_NEAREST);
}

// ===========================================================

// ===========================================================

double PointToLineDistance(CPoint pt, CPoint lineStart, CPoint lineEnd)
{
    double dx = (double)(lineEnd.x - lineStart.x);
    double dy = (double)(lineEnd.y - lineStart.y);
    double lenSq = dx*dx + dy*dy;
    if (lenSq < 1e-6) return Distance(pt, lineStart);

    double t = ((pt.x - lineStart.x)*dx + (pt.y - lineStart.y)*dy) / lenSq;
    t = max(0.0, min(1.0, t));

    double projX = lineStart.x + t * dx;
    double projY = lineStart.y + t * dy;
    return Distance(pt, CPoint((int)projX, (int)projY));
}

bool PointOnRectEdge(CPoint pt, CRect rc, int tolerance)
{
    rc.InflateRect(tolerance, tolerance);
    if (!rc.PtInRect(pt)) return false;
    rc.InflateRect(-tolerance, -tolerance);
    rc.InflateRect(-tolerance, -tolerance);
    return !rc.PtInRect(pt);
}
