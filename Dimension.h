#pragma once
#include "Entity.h"

class CDimLinearEntity : public CEntity
{
	DECLARE_SERIAL(CDimLinearEntity)
public:
	CDimLinearEntity();
	enum DimMode { DIM_ALIGNED = 0, DIM_HORIZONTAL = 1, DIM_VERTICAL = 2 };
	CDimLinearEntity(CPoint p1, CPoint p2, DimMode mode = DIM_ALIGNED, double offset = 0.0, CPoint place = CPoint(0,0));

	CPoint m_pt1, m_pt2;
	DimMode m_mode;
	double  m_offset; // signed offset in world units (positive = normal direction used in Draw)
	CPoint  m_place; // placement point chosen by user (world coords), used for H/V modes
	CPoint  m_ptText; // explicit text placement chosen by user (world coords). (0,0)=unset
	bool    m_bTextPlaced; // whether text has been confirmed by user

	virtual void   Draw(CDC* pDC, double scale, CPoint offset);
	virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
	virtual void   Move(double dx, double dy);
	virtual void   Rotate(CPoint base, double angle);
	virtual void   Scale(CPoint base, double factor);
	virtual void   Mirror(CPoint p1, CPoint p2);
	virtual CRect  GetBounds();
	virtual CEntity* Clone() const;
	virtual void   Serialize(CArchive& ar);
};

class CDimAngularEntity : public CEntity
{
	DECLARE_SERIAL(CDimAngularEntity)
public:
	CDimAngularEntity();
	CDimAngularEntity(CPoint center, CPoint p1, CPoint p2, CPoint place = CPoint(0,0));
	CDimAngularEntity(CPoint center, CPoint p1, CPoint p2, CPoint place, double ang1, double ang2);

	CPoint m_ptCenter, m_pt1, m_pt2;
	CPoint m_ptPlace; // user-chosen point to place arc through
	CPoint m_ptText; // explicit text placement chosen by user (world coords). (0,0)=unset
	bool   m_bTextPlaced; // whether text has been confirmed by user
	double m_ang1, m_ang2; // stored angles for stable rendering
	double m_v1x, m_v1y, m_v2x, m_v2y; // unit direction vectors from center along the two rays

	virtual void   Draw(CDC* pDC, double scale, CPoint offset);
	virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
	virtual void   Move(double dx, double dy);
	virtual void   Rotate(CPoint base, double angle);
	virtual void   Scale(CPoint base, double factor);
	virtual void   Mirror(CPoint p1, CPoint p2);
	virtual CRect  GetBounds();
	virtual CEntity* Clone() const;
	virtual void   Serialize(CArchive& ar);
};

class CDimRadiusEntity : public CEntity
{
	DECLARE_SERIAL(CDimRadiusEntity)
public:
	CDimRadiusEntity();
	CDimRadiusEntity(CPoint center, int radius, double angle);

	CPoint m_ptCenter;
	int    m_nRadius;
	double m_dAngle;
	CPoint m_ptLeaderEnd;
	CPoint m_ptText;
	bool   m_bTextPlaced;

	virtual void   Draw(CDC* pDC, double scale, CPoint offset);
	virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
	virtual void   Move(double dx, double dy);
	virtual void   Rotate(CPoint base, double angle);
	virtual void   Scale(CPoint base, double factor);
	virtual void   Mirror(CPoint p1, CPoint p2);
	virtual CRect  GetBounds();
	virtual CEntity* Clone() const;
	virtual void   Serialize(CArchive& ar);
};

class CDimDiamEntity : public CEntity
{
	DECLARE_SERIAL(CDimDiamEntity)
public:
	CDimDiamEntity();
	CDimDiamEntity(CPoint center, int radius, double angle);

	CPoint m_ptCenter;
	int    m_nRadius;
	double m_dAngle;
	CPoint m_ptEnd1;
	CPoint m_ptEnd2;
	CPoint m_ptText;
	bool   m_bTextPlaced;

	virtual void   Draw(CDC* pDC, double scale, CPoint offset);
	virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
	virtual void   Move(double dx, double dy);
	virtual void   Rotate(CPoint base, double angle);
	virtual void   Scale(CPoint base, double factor);
	virtual void   Mirror(CPoint p1, CPoint p2);
	virtual CRect  GetBounds();
	virtual CEntity* Clone() const;
	virtual void   Serialize(CArchive& ar);
};

class CDimArcLengthEntity : public CEntity
{
	DECLARE_SERIAL(CDimArcLengthEntity)
public:
	CDimArcLengthEntity();
	CDimArcLengthEntity(CPoint center, int arcRadius, int dimRadius, double startAngle, double endAngle, double sweep);

	CPoint m_ptCenter;
	int    m_nArcRadius;
	int    m_nDimRadius;
	double m_dStartAngle;
	double m_dEndAngle;
	double m_dSweep;
	CPoint m_ptText;
	bool   m_bTextPlaced;

	virtual void   Draw(CDC* pDC, double scale, CPoint offset);
	virtual bool   HitTest(CPoint pt, double scale, CPoint offset);
	virtual void   Move(double dx, double dy);
	virtual void   Rotate(CPoint base, double angle);
	virtual void   Scale(CPoint base, double factor);
	virtual void   Mirror(CPoint p1, CPoint p2);
	virtual CRect  GetBounds();
	virtual CEntity* Clone() const;
	virtual void   Serialize(CArchive& ar);
};
