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
    , m_dModelUnitScale(1.0)
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
    , m_bObjectSnap(true)
    , m_bSnapEndpoint(true)
    , m_bSnapMidpoint(true)
    , m_bSnapCenter(true)
    , m_bSnapQuadrant(true)
    , m_bSnapNearest(false)
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
    m_dModelUnitScale = 1.0;
    m_ptOffset = CPoint(0, 0);
    m_bModified = false;
    m_drawState = STATE_IDLE;
    m_strCommandPrompt = L"Command: ";
    m_layers.clear();
    m_layers.push_back(L"0");
    m_strCurrentLayer = L"0";

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

std::vector<CEntity*> CLargeHWDoc::HitTestEntities(CPoint pt, double scale, CPoint offset) const
{
    std::vector<CEntity*> hits;
    for (auto it = m_entities.rbegin(); it != m_entities.rend(); ++it) {
        CEntity* p = *it;
        if (p->m_bVisible && p->HitTest(pt, scale, offset))
            hits.push_back(p);
    }
    return hits;
}

int CLargeHWDoc::SelectByPoint(CPoint pt, double scale, CPoint offset)
{
    DeselectAll();
    std::vector<CEntity*> hits = HitTestEntities(pt, scale, offset);
    if (hits.empty())
        return 0;

    hits.front()->m_bSelected = true;
    return 1;
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
        CRect screenBounds = p->ToScreenRect(bounds, scale, offset);
        int hitTolerance = max(3, p->m_nLineWidth + 2);
        screenBounds.InflateRect(hitTolerance, hitTolerance);
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
    CAction act;
    act.type = CAction::ACT_DELETE_MULTI;
    act.pEntity = nullptr;

    for (auto it = m_entities.begin(); it != m_entities.end(); ) {
        if ((*it)->m_bSelected) {
            act.entityIDs.push_back((*it)->m_nID);
            act.savedEntities.push_back((*it)->Clone());
            delete *it;
            it = m_entities.erase(it);
            m_bModified = true;
        } else {
            ++it;
        }
    }

    if (!act.entityIDs.empty())
        PushUndo(act);

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
    TRACE(L"[DEBUG] MoveSelected: dx=%.0f, dy=%.0f, totalEntities=%d\n", dx, dy, (int)m_entities.size());
}

void CLargeHWDoc::EraseSelected()
{
    DeleteSelected();
}

void CLargeHWDoc::RotateSelected(CPoint base, double angle)
{
    for (auto* p : m_entities)
        if (p->m_bSelected) p->Rotate(base, angle);
    m_bModified = true;
    SetModifiedFlag();
    TRACE(L"[DEBUG] RotateSelected: base=(%d,%d), angle=%.3f rad (%.1f deg)\n", base.x, base.y, angle, angle * 180.0 / M_PI);
}

void CLargeHWDoc::ScaleSelected(CPoint base, double factor)
{
    for (auto* p : m_entities)
        if (p->m_bSelected) p->Scale(base, factor);
    m_bModified = true;
    SetModifiedFlag();
    TRACE(L"[DEBUG] ScaleSelected: base=(%d,%d), factor=%.3f\n", base.x, base.y, factor);
}

void CLargeHWDoc::MirrorSelected(CPoint p1, CPoint p2)
{
    for (auto* p : m_entities)
        if (p->m_bSelected) p->Mirror(p1, p2);
    m_bModified = true;
    SetModifiedFlag();
    TRACE(L"[DEBUG] MirrorSelected: p1=(%d,%d), p2=(%d,%d)\n", p1.x, p1.y, p2.x, p2.y);
}

void CLargeHWDoc::CollectSnapPoints(std::vector<CPoint>& points, std::vector<SnapType>& types) const
{
    points.clear();
    types.clear();
    for (const auto* p : m_entities) {
        if (!p->m_bVisible) continue;
        const_cast<CEntity*>(p)->GetSnapPoints(points, types);
    }
}

void CLargeHWDoc::RecordGripUndo(CEntity* pEntity)
{
    if (!pEntity) return;
    CAction act;
    act.type = CAction::ACT_MODIFY;
    act.pEntity = pEntity->Clone();
    act.nEntityID = pEntity->m_nID;
    PushUndo(act);
}

void CLargeHWDoc::RecordModifyUndo()
{
    if (GetSelectedCount() == 0) return;

    CAction act;
    act.type = CAction::ACT_MODIFY_MULTI;
    act.pEntity = nullptr;
    for (auto* p : m_entities) {
        if (p->m_bSelected) {
            act.entityIDs.push_back(p->m_nID);
            act.savedEntities.push_back(p->Clone());
        }
    }
    PushUndo(act);
}

void CLargeHWDoc::RecordCreateUndo(const std::vector<int>& entityIDs)
{
    if (entityIDs.empty()) return;

    CAction act;
    act.type = CAction::ACT_CREATE_MULTI;
    act.pEntity = nullptr;
    act.entityIDs = entityIDs;
    PushUndo(act);
}

void CLargeHWDoc::AddLayer(const CString& layer)
{
    for (const auto& l : m_layers)
        if (l == layer) return;
    m_layers.push_back(layer);
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
    for (auto& act : m_undoStack) {
        if (act.pEntity) { delete act.pEntity; act.pEntity = nullptr; }
        for (auto* p : act.savedEntities) delete p;
        act.savedEntities.clear();
    }
    m_undoStack.clear();
}

void CLargeHWDoc::ClearRedoStack()
{
    for (auto& act : m_redoStack) {
        if (act.pEntity) { delete act.pEntity; act.pEntity = nullptr; }
        for (auto* p : act.savedEntities) delete p;
        act.savedEntities.clear();
    }
    m_redoStack.clear();
}

void CLargeHWDoc::Undo()
{
    if (m_undoStack.empty()) {
        m_strCommandPrompt = L"Nothing to undo";
        return;
    }

    CAction action = m_undoStack.back();
    m_undoStack.pop_back();

    CAction reverseAction;
    reverseAction.nEntityID = action.nEntityID;

    switch (action.type) {
    case CAction::ACT_ADD: {
        reverseAction.type = CAction::ACT_DELETE;
        CEntity* pFound = FindEntityByID(action.nEntityID);
        if (pFound) {
            reverseAction.pEntity = pFound->Clone();
            RemoveEntity(action.nEntityID, false);
            m_strCommandPrompt.Format(L"Undo: removed entity #%d", action.nEntityID);
        } else {
            m_strCommandPrompt = L"Undo: entity not found";
            m_undoStack.push_back(action);
            return;
        }
        break;
    }
    case CAction::ACT_DELETE: {
        reverseAction.type = CAction::ACT_ADD;
        reverseAction.pEntity = action.pEntity->Clone();
        CEntity* restore = action.pEntity->Clone();
        restore->m_nID = action.nEntityID;
        m_entities.push_back(restore);
        m_strCommandPrompt.Format(L"Undo: restored entity #%d", action.nEntityID);
        break;
    }
    case CAction::ACT_MODIFY: {
        reverseAction.type = CAction::ACT_MODIFY;
        CEntity* pCurrent = FindEntityByID(action.nEntityID);
        if (pCurrent) {
            reverseAction.pEntity = pCurrent->Clone();
            for (size_t i = 0; i < m_entities.size(); ++i) {
                if (m_entities[i]->m_nID == action.nEntityID) {
                    delete m_entities[i];
                    m_entities[i] = action.pEntity->Clone();
                    m_entities[i]->m_nID = action.nEntityID;
                    break;
                }
            }
            m_strCommandPrompt.Format(L"Undo: reverted entity #%d", action.nEntityID);
        } else {
            m_strCommandPrompt = L"Undo: entity not found";
            m_undoStack.push_back(action);
            return;
        }
        break;
    }
    case CAction::ACT_CREATE_MULTI: {
        reverseAction.type = CAction::ACT_DELETE_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (int id : action.entityIDs) {
            CEntity* pFound = FindEntityByID(id);
            if (pFound) {
                reverseAction.savedEntities.push_back(pFound->Clone());
                RemoveEntity(id, false);
            }
        }
        m_strCommandPrompt.Format(L"Undo: removed %d entities", (int)action.entityIDs.size());
        break;
    }
    case CAction::ACT_DELETE_MULTI: {
        reverseAction.type = CAction::ACT_CREATE_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (size_t i = 0; i < action.entityIDs.size() && i < action.savedEntities.size(); ++i) {
            CEntity* restore = action.savedEntities[i]->Clone();
            restore->m_nID = action.entityIDs[i];
            m_entities.push_back(restore);
        }
        m_strCommandPrompt.Format(L"Undo: restored %d entities", (int)action.entityIDs.size());
        break;
    }
    case CAction::ACT_MODIFY_MULTI: {
        reverseAction.type = CAction::ACT_MODIFY_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (size_t i = 0; i < action.entityIDs.size() && i < action.savedEntities.size(); ++i) {
            int id = action.entityIDs[i];
            CEntity* pCurrent = FindEntityByID(id);
            if (pCurrent) {
                reverseAction.savedEntities.push_back(pCurrent->Clone());
                for (size_t j = 0; j < m_entities.size(); ++j) {
                    if (m_entities[j]->m_nID == id) {
                        delete m_entities[j];
                        m_entities[j] = action.savedEntities[i]->Clone();
                        m_entities[j]->m_nID = id;
                        break;
                    }
                }
            }
        }
        m_strCommandPrompt.Format(L"Undo: reverted %d entities", (int)action.entityIDs.size());
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
    if (m_redoStack.empty()) {
        m_strCommandPrompt = L"Nothing to redo";
        return;
    }

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
        m_strCommandPrompt.Format(L"Redo: restored entity #%d", action.nEntityID);
        break;
    }
    case CAction::ACT_DELETE: {
        reverseAction.type = CAction::ACT_ADD;
        CEntity* pFound = FindEntityByID(action.nEntityID);
        if (pFound) {
            reverseAction.pEntity = pFound->Clone();
            RemoveEntity(action.nEntityID, false);
            m_strCommandPrompt.Format(L"Redo: removed entity #%d", action.nEntityID);
        } else {
            m_strCommandPrompt = L"Redo: entity not found";
            m_redoStack.push_back(action);
            return;
        }
        break;
    }
    case CAction::ACT_MODIFY: {
        reverseAction.type = CAction::ACT_MODIFY;
        CEntity* pCurrent = FindEntityByID(action.nEntityID);
        if (pCurrent) {
            reverseAction.pEntity = pCurrent->Clone();
            for (size_t i = 0; i < m_entities.size(); ++i) {
                if (m_entities[i]->m_nID == action.nEntityID) {
                    delete m_entities[i];
                    m_entities[i] = action.pEntity->Clone();
                    m_entities[i]->m_nID = action.nEntityID;
                    break;
                }
            }
            m_strCommandPrompt.Format(L"Redo: reverted entity #%d", action.nEntityID);
        } else {
            m_strCommandPrompt = L"Redo: entity not found";
            m_redoStack.push_back(action);
            return;
        }
        break;
    }
    case CAction::ACT_CREATE_MULTI: {
        reverseAction.type = CAction::ACT_DELETE_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (size_t i = 0; i < action.entityIDs.size() && i < action.savedEntities.size(); ++i) {
            CEntity* restore = action.savedEntities[i]->Clone();
            restore->m_nID = action.entityIDs[i];
            m_entities.push_back(restore);
        }
        m_strCommandPrompt.Format(L"Redo: restored %d entities", (int)action.entityIDs.size());
        break;
    }
    case CAction::ACT_DELETE_MULTI: {
        reverseAction.type = CAction::ACT_CREATE_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (int id : action.entityIDs) {
            CEntity* pFound = FindEntityByID(id);
            if (pFound) {
                reverseAction.savedEntities.push_back(pFound->Clone());
                RemoveEntity(id, false);
            }
        }
        m_strCommandPrompt.Format(L"Redo: removed %d entities", (int)action.entityIDs.size());
        break;
    }
    case CAction::ACT_MODIFY_MULTI: {
        reverseAction.type = CAction::ACT_MODIFY_MULTI;
        reverseAction.entityIDs = action.entityIDs;
        reverseAction.pEntity = nullptr;
        for (size_t i = 0; i < action.entityIDs.size() && i < action.savedEntities.size(); ++i) {
            int id = action.entityIDs[i];
            CEntity* pCurrent = FindEntityByID(id);
            if (pCurrent) {
                reverseAction.savedEntities.push_back(pCurrent->Clone());
                for (size_t j = 0; j < m_entities.size(); ++j) {
                    if (m_entities[j]->m_nID == id) {
                        delete m_entities[j];
                        m_entities[j] = action.savedEntities[i]->Clone();
                        m_entities[j]->m_nID = id;
                        break;
                    }
                }
            }
        }
        m_strCommandPrompt.Format(L"Redo: reverted %d entities", (int)action.entityIDs.size());
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
        ar << m_dModelUnitScale;
    } else {
        RemoveAllEntities();
        ClearUndoStack();
        ClearRedoStack();
        m_dModelUnitScale = 1.0;

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
            case ENT_POINT:     pEntity = new CPointEntity(); break;
            default: continue;
            }

            pEntity->Serialize(ar);
            m_entities.push_back(pEntity);
        }

        CFile* pFile = ar.GetFile();
        if (pFile && pFile->GetPosition() < pFile->GetLength()) {
            ar >> m_dModelUnitScale;
            if (m_dModelUnitScale < 1.0)
                m_dModelUnitScale = 1.0;
        }

        m_bModified = false;
    }
}

#ifdef _DEBUG
void CLargeHWDoc::AssertValid() const { CDocument::AssertValid(); }
void CLargeHWDoc::Dump(CDumpContext& dc) const { CDocument::Dump(dc); }
#endif
