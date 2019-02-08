#include "PrintHostDialogs.hpp"

#include <algorithm>

#include <wx/frame.h>
#include <wx/progdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/checkbox.h>
#include <wx/button.h>
#include <wx/dataview.h>
#include <wx/wupdlock.h>
#include <wx/debug.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "MsgDialog.hpp"
#include "I18N.hpp"
#include "../Utils/PrintHost.hpp"

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {


PrintHostSendDialog::PrintHostSendDialog(const fs::path &path)
    : MsgDialog(nullptr, _(L("Send G-Code to printer host")), _(L("Upload to Printer Host with the following filename:")), wxID_NONE)
    , txt_filename(new wxTextCtrl(this, wxID_ANY, path.filename().wstring()))
    , box_print(new wxCheckBox(this, wxID_ANY, _(L("Start printing after upload"))))
{
#ifdef __APPLE__
    txt_filename->OSXDisableAllSmartSubstitutions();
#endif

    auto *label_dir_hint = new wxStaticText(this, wxID_ANY, _(L("Use forward slashes ( / ) as a directory separator if needed.")));
    label_dir_hint->Wrap(CONTENT_WIDTH);

    content_sizer->Add(txt_filename, 0, wxEXPAND);
    content_sizer->Add(label_dir_hint);
    content_sizer->AddSpacer(VERT_SPACING);
    content_sizer->Add(box_print, 0, wxBOTTOM, 2*VERT_SPACING);

    btn_sizer->Add(CreateStdDialogButtonSizer(wxOK | wxCANCEL));

    txt_filename->SetFocus();
    wxString stem(path.stem().wstring());
    txt_filename->SetSelection(0, stem.Length());

    Fit();
}

fs::path PrintHostSendDialog::filename() const
{
    return into_path(txt_filename->GetValue());
}

bool PrintHostSendDialog::start_print() const
{
    return box_print->GetValue();
}



wxDEFINE_EVENT(EVT_PRINTHOST_PROGRESS, PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_ERROR,    PrintHostQueueDialog::Event);
wxDEFINE_EVENT(EVT_PRINTHOST_CANCEL,   PrintHostQueueDialog::Event);

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id)
    : wxEvent(winid, eventType)
    , job_id(job_id)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, int progress)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , progress(progress)
{}

PrintHostQueueDialog::Event::Event(wxEventType eventType, int winid, size_t job_id, wxString error)
    : wxEvent(winid, eventType)
    , job_id(job_id)
    , error(std::move(error))
{}

wxEvent *PrintHostQueueDialog::Event::Clone() const
{
    return new Event(*this);
}

PrintHostQueueDialog::PrintHostQueueDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _(L("Print host upload queue")), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , on_progress_evt(this, EVT_PRINTHOST_PROGRESS, &PrintHostQueueDialog::on_progress, this)
    , on_error_evt(this, EVT_PRINTHOST_ERROR, &PrintHostQueueDialog::on_error, this)
    , on_cancel_evt(this, EVT_PRINTHOST_CANCEL, &PrintHostQueueDialog::on_cancel, this)
{
    enum { HEIGHT = 800, WIDTH = 400, SPACING = 5 };

    SetSize(wxSize(HEIGHT, WIDTH));
    SetSize(GetMinSize());

    auto *topsizer = new wxBoxSizer(wxVERTICAL);

    job_list = new wxDataViewListCtrl(this, wxID_ANY);
    // Note: Keep these in sync with Column
    job_list->AppendTextColumn("ID", wxDATAVIEW_CELL_INERT);
    job_list->AppendProgressColumn("Progress", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("Status", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("Host", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("Filename", wxDATAVIEW_CELL_INERT);
    job_list->AppendTextColumn("error_message", wxDATAVIEW_CELL_INERT, -1, wxALIGN_CENTER, wxDATAVIEW_COL_HIDDEN);

    auto *btnsizer = new wxBoxSizer(wxHORIZONTAL);
    btn_cancel = new wxButton(this, wxID_DELETE, _(L("Cancel selected")));
    btn_cancel->Disable();
    btn_error = new wxButton(this, wxID_ANY, _(L("Show error message")));
    btn_error->Disable();
    auto *btn_close = new wxButton(this, wxID_CANCEL, _(L("Close")));
    btnsizer->Add(btn_cancel, 0, wxRIGHT, SPACING);
    btnsizer->Add(btn_error, 0);
    btnsizer->AddStretchSpacer();
    btnsizer->Add(btn_close);

    topsizer->Add(job_list, 1, wxEXPAND | wxBOTTOM, SPACING);
    topsizer->Add(btnsizer, 0, wxEXPAND);
    SetSizer(topsizer);

    job_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { on_list_select(); });

    btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }

        const JobState state = get_state(selected);
        if (state < ST_ERROR) {
            // TODO: cancel
            GUI::wxGetApp().printhost_job_queue().cancel(selected);
        }
    });

    btn_error->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        int selected = job_list->GetSelectedRow();
        if (selected == wxNOT_FOUND) { return; }
        GUI::show_error(nullptr, job_list->GetTextValue(selected, COL_ERRORMSG));
    });
}

void PrintHostQueueDialog::append_job(const PrintHostJob &job)
{
    wxCHECK_RET(!job.empty(), "PrintHostQueueDialog: Attempt to append an empty job");

    wxVector<wxVariant> fields;
    fields.push_back(wxVariant(wxString::Format("%d", job_list->GetItemCount() + 1)));
    fields.push_back(wxVariant(0));
    fields.push_back(wxVariant(_(L("Enqueued"))));
    fields.push_back(wxVariant(job.printhost->get_host()));
    fields.push_back(wxVariant(job.upload_data.upload_path.string()));
    fields.push_back(wxVariant(""));
    job_list->AppendItem(fields, static_cast<wxUIntPtr>(ST_NEW));
}

PrintHostQueueDialog::JobState PrintHostQueueDialog::get_state(int idx)
{
    wxCHECK_MSG(idx >= 0 && idx < job_list->GetItemCount(), ST_ERROR, "Out of bounds access to job list");
    return static_cast<JobState>(job_list->GetItemData(job_list->RowToItem(idx)));
}

void PrintHostQueueDialog::set_state(int idx, JobState state)
{
    wxCHECK_RET(idx >= 0 && idx < job_list->GetItemCount(), "Out of bounds access to job list");
    job_list->SetItemData(job_list->RowToItem(idx), static_cast<wxUIntPtr>(state));

    switch (state) {
        case ST_NEW:        job_list->SetValue(_(L("Enqueued")), idx, COL_STATUS); break;
        case ST_PROGRESS:   job_list->SetValue(_(L("Uploading")), idx, COL_STATUS); break;
        case ST_ERROR:      job_list->SetValue(_(L("Error")), idx, COL_STATUS); break;
        case ST_CANCELLING: job_list->SetValue(_(L("Cancelling")), idx, COL_STATUS); break;
        case ST_CANCELLED:  job_list->SetValue(_(L("Cancelled")), idx, COL_STATUS); break;
        case ST_COMPLETED:  job_list->SetValue(_(L("Completed")), idx, COL_STATUS); break;
    }
}

void PrintHostQueueDialog::on_list_select()
{
    int selected = job_list->GetSelectedRow();
    if (selected != wxNOT_FOUND) {
        const JobState state = get_state(selected);
        btn_cancel->Enable(state < ST_ERROR);
        btn_error->Enable(state == ST_ERROR);
        Layout();
    } else {
        btn_cancel->Disable();
    }
}

void PrintHostQueueDialog::on_progress(Event &evt)
{
    wxCHECK_RET(evt.job_id < job_list->GetItemCount(), "Out of bounds access to job list");

    if (evt.progress < 100) {
        set_state(evt.job_id, ST_PROGRESS);
        job_list->SetValue(wxVariant(evt.progress), evt.job_id, COL_PROGRESS);
    } else {
        set_state(evt.job_id, ST_COMPLETED);
        job_list->SetValue(wxVariant(100), evt.job_id, COL_PROGRESS);
    }

    on_list_select();
}

void PrintHostQueueDialog::on_error(Event &evt)
{
    wxCHECK_RET(evt.job_id < job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_ERROR);

    auto errormsg = wxString::Format("%s\n%s", _(L("Error uploading to print host:")), evt.error);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);
    job_list->SetValue(wxVariant(errormsg), evt.job_id, COL_ERRORMSG);    // Stashes the error message into a hidden column for later

    on_list_select();

    GUI::show_error(nullptr, std::move(errormsg));
}

void PrintHostQueueDialog::on_cancel(Event &evt)
{
    wxCHECK_RET(evt.job_id < job_list->GetItemCount(), "Out of bounds access to job list");

    set_state(evt.job_id, ST_CANCELLED);
    job_list->SetValue(wxVariant(0), evt.job_id, COL_PROGRESS);

    on_list_select();
}


}}
