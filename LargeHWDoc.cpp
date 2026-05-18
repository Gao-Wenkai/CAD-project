// LargeHWDoc.cpp: CAD Document Implementation

#include "pch.h"
#include "framework.h"
#ifndef SHARED_HANDLERS
#include "LargeHW.h"
#endif

#include "LargeHWDoc.h"
#include <propkey.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

IMPLEMENT_DYNCREATE(CLargeHWDoc, CDocument)

BEGIN_MESSAGE_MAP(CLargeHWDoc, CDocument)
END_MESSAGE_MAP()

CLargeHWDoc::CLargeHWDoc() noexcept
    : m_dScale(1.0)
    , m_ptOffset(0, 0)
    , m_bShowGrid(true)
    , m_bSnapToGrid(false)
    , m_bOrthoMode(false)
    , m_nGridSpacing(50)
    , m_drawState(STATE_IDLE)
    , m_strCommandPrompt(L"Command: ")
    , m_currentColor(RGB(0, 255, 209))
    , m_currentLineStyle(PS_SOLID)
    , m_currentLineWidth(1)
    , m_strCurrentLayer(L"0")
    , m_bModified(false)
{
}

CLargeHWDoc::~CLargeHWDoc()
{
    RemoveAllEntities();
}

BOOL CLargeHWDoc::OnNewDocument()
{
    if (!CDocument::OnNewDocument())
        return FALSE;

    RemoveAllEntities();
    ClearUndoStack();
    ClearRedoStack();
    m_dScale = 1.0;
    m_ptOffset = CPoint(0, 0);
    m_bModified = false;
    m_drawState = STATE_IDLE;
    m_strCommandPrompt = L"Command: ";

    return TRUE;
}

// ============================================================
// Entity management
// ============================================================

void CLargeHWDoc::AddEntity(CEntity* pEntity, bool bRecordUndo)
{
    if (!pEntity) return;

    pEntity->m_color = m_currentColor;
    pEntity->m_nLineStyle = m_currentLineStyle;
    pEntity->m_nLineWidth = m_currentLineWidth;
    pEntity->m_strLayer = m_strCurrentLayer;

    m_entities.push_back(pEntity);

    if (bRecordUndo) {
        CAction act;
        act.type = CAction::ACT_ADD;
        act.pEntity = pEntity->Clone();
        act.nEntityID = pEntity->m_nID;
        PushUndo(act);
    }

    m_bModified = true;
    SetModifiedFlag();
}

void CLargeHWDoc::RemoveEntity(int nEntityID, bool bRecordUndo)
{
    for (auto it = m_entities.begin(); it != m_entities.end(); ++it) {
        if ((*it)->m_nID == nEntityID) {
            if (bRecordUndo) {
                CAction act;
                act.type = CAction::ACT_DELETE;
                act.pEntity = (*it)->Clone();
                act.nEntityID = nEntityID;
                PushUndo(act);
            }
            delete *it;
            m_entities.erase(it);
            m_bModified = true;
            SetModifiedFlag();
            return;
        }
    }
}

void CLargeHWDoc::RemoveAllEntities()
{
    for (auto* p : m_entities) delete p;
    m_entities.clear();
}

CEntity* CLargeHWDoc::FindEntityByID(int nID) const
{
    for (auto* p : m_entities)
        if (p->m_nID == nID) return p;
    return nullptr;
}

CEntity* CLargeHWDoc::HitTestEntity(CPoint pt, double scale, CPoint offset) const
{
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it) {
        CEntity* p = *it;
        if (p->m_bVisible && p->HitTest(pt, scale, offset)) {
            int gripIdx = p->HitTestGrip(pt, scale, offset);
            if (gripIdx >= 0) return p;
            return p;
        }
    }
    return nullptr;
}

int CLargeHWDoc::SelectByPoint(CPoint pt, double scale, CPoint offset)
{
    DeselectAll();
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it) {
        CEntity* p = *it;
        if (p->m_bVisible && p->HitTest(pt, scale, offset)) {
            p->m_bSelected = true;
            return 1;
        }
    }
    return 0;
}

int CLargeHWDoc::SelectByWindow(CRect rcWindow, double scale, CPoint offset)
{
    DeselectAll();
    CRect rc = rcWindow;
    rc.NormalizeRect();
    int nCount = 0;
    for (auto* p : m_entities) {
        if (!p->m_bVisible) continue;
        CRect bounds = p->GetBounds();
        CRect screenBounds(
            (int)(bounds.left * scale + offset.x),
            (int)(bounds.top * scale + offset.y),
            (int)(bounds.right * scale + offset.x),
            (int)(bounds.bottom * scale + offset.y)
        );
        screenBounds.NormalizeRect();
        CRect intersection;
        if (intersection.IntersectRect(rc, screenBounds)) {
            p->m_bSelected = true;
            nCount++;
        }
    }
    return nCount;
}

void CLargeHWDoc::DeselectAll()
{
    for (auto* p : m_entities) p->m_bSelected = false;
}

void CLargeHWDoc::DeleteSelected()
{
    for (auto it = m_entities.begin(); it != m_entities.end(); ) {
        if ((*it)->m_bSelected) {
            CAction act;
            act.type = CAction::ACT_DELETE;
            act.pEntity = (*it)->Clone();
            act.nEntityID = (*it)->m_nID;
            PushUndo(act);

            delete *it;
            it = m_entities.erase(it);
            m_bModified = true;
        } else {
            ++it;
        }
    }
    SetModifiedFlag();
}

int CLargeHWDoc::GetSelectedCount() const
{
    int n = 0;
    for (const auto* p : m_entities)
        if (p->m_bSelected) n++;
    return n;
}

void CLargeHWDoc::MoveSelected(double dx, double dy)
{
    for (auto* p : m_entities)
        if (p->m_bSelected) p->Move(dx, dy);
    m_bModified = true;
    SetModifiedFlag();
}

void CLargeHWDoc::EraseSelected()
{
    DeleteSelected();
}

std::vector<CEntity*> CLargeHWDoc::GetSelectedEntities() const
{
    std::vector<CEntity*> sel;
    for (auto* p : m_entities)
        if (p->m_bSelected) sel.push_back(p);
    return sel;
}

// ============================================================
// Undo/Redo
// ============================================================

void CLargeHWDoc::PushUndo(const CAction& action)
{
    m_undoStack.push_back(action);
    if ((int)m_undoStack.size() > MAX_UNDO) m_undoStack.pop_front();
    ClearRedoStack();
}

void CLargeHWDoc::ClearUndoStack()
{
    for (auto& act : m_undoStack)
        if (act.pEntity) delete act.pEntity;
    m_undoStack.clear();
}

void CLargeHWDoc::ClearRedoStack()
{
    for (auto& act : m_redoStack)
        if (act.pEntity) delete act.pEntity;
    m_redoStack.clear();
}

void CLargeHWDoc::Undo()
{
    if (m_undoStack.empty()) return;

    CAction action = m_undoStack.back();
    m_undoStack.pop_back();

    CAction reverseAction;
    reverseAction.nEntityID = action.nEntityID;

    switch (action.type) {
    case CAction::ACT_ADD: {
        reverseAction.type = CAction::ACT_DELETE;
        reverseAction.pEntity = FindEntityByID(action.nEntityID)->Clone();
        RemoveEntity(action.nEntityID, false);
        break;
    }
    case CAction::ACT_DELETE: {
        reverseAction.type = CAction::ACT_ADD;
        reverseAction.pEntity = action.pEntity->Clone();
        CEntity* restore = action.pEntity->Clone();
        restore->m_nID = action.nEntityID;
        m_entities.push_back(restore);
        break;
    }
    default:
        break;
    }

    m_redoStack.push_back(reverseAction);
    if ((int)m_redoStack.size() > MAX_UNDO) m_redoStack.pop_front();

    m_bModified = true;
    SetModifiedFlag();
    UpdateAllViews(nullptr);
}

void CLargeHWDoc::Redo()
{
    if (m_redoStack.empty()) return;

    CAction action = m_redoStack.back();
    m_redoStack.pop_back();

    CAction reverseAction;
    reverseAction.nEntityID = action.nEntityID;

    switch (action.type) {
    case CAction::ACT_ADD: {
        reverseAction.type = CAction::ACT_DELETE;
        reverseAction.pEntity = action.pEntity->Clone();
        CEntity* restore = action.pEntity->Clone();
        restore->m_nID = action.nEntityID;
        m_entities.push_back(restore);
        break;
    }
    case CAction::ACT_DELETE: {
        reverseAction.type = CAction::ACT_ADD;
        reverseAction.pEntity = FindEntityByID(action.nEntityID)->Clone();
        RemoveEntity(action.nEntityID, false);
        break;
    }
    default:
        break;
    }

    m_undoStack.push_back(reverseAction);

    m_bModified = true;
    SetModifiedFlag();
    UpdateAllViews(nullptr);
}

// ============================================================
// Serialization
// ============================================================

void CLargeHWDoc::Serialize(CArchive& ar)
{
    if (ar.IsStoring()) {
        int nCount = (int)m_entities.size();
        ar << nCount;
        ar << m_dScale << m_ptOffset.x << m_ptOffset.y;
        ar << m_currentColor << m_currentLineStyle << m_currentLineWidth;
        ar << m_strCurrentLayer;

        for (auto* p : m_entities) {
            ar << (int)p->m_Type;
            p->Serialize(ar);
        }
    } else {
        RemoveAllEntities();
        ClearUndoStack();
        ClearRedoStack();

        int nCount = 0;
        ar >> nCount;
        ar >> m_dScale >> m_ptOffset.x >> m_ptOffset.y;
        ar >> m_currentColor >> m_currentLineStyle >> m_currentLineWidth;
        ar >> m_strCurrentLayer;

        for (int i = 0; i < nCount; ++i) {
            int nType;
            ar >> nType;
            CEntity* pEntity = nullptr;

            switch ((EntityType)nType) {
            case ENT_LINE:      pEntity = new CLineEntity(); break;
            case ENT_CIRCLE:    pEntity = new CCircleEntity(); break;
            case ENT_ARC:       pEntity = new CArcEntity(); break;
            case ENT_RECTANGLE: pEntity = new CRectangleEntity(); break;
            case ENT_POLYGON:   pEntity = new CPolygonEntity(); break;
            case ENT_ELLIPSE:   pEntity = new CEllipseEntity(); break;
            case ENT_POLYLINE:  pEntity = new CPolylineEntity(); break;
            case ENT_TEXT:      pEntity = new CTextEntity(); break;
            default: continue;
            }

            pEntity->Serialize(ar);
            m_entities.push_back(pEntity);
        }

        m_bModified = false;
    }
}

#ifdef _DEBUG
void CLargeHWDoc::AssertValid() const { CDocument::AssertValid(); }
void CLargeHWDoc::Dump(CDumpContext& dc) const { CDocument::Dump(dc); }
#endif
