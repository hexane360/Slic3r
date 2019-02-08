#include "Plater.hpp"

#include <cstddef>
#include <algorithm>
#include <vector>
#include <string>
#include <regex>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/optional.hpp>
#include <boost/filesystem/path.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/notebook.h>
#include <wx/button.h>
#include <wx/bmpcbox.h>
#include <wx/statbox.h>
#include <wx/statbmp.h>
#include <wx/filedlg.h>
#include <wx/dnd.h>
#include <wx/progdlg.h>
#include <wx/wupdlock.h>
#include <wx/colordlg.h>
#include <wx/numdlg.h>
#include <wx/debug.h>

#include "libslic3r/libslic3r.h"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Format/AMF.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/GCode/PreviewData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLA/SLARotfinder.hpp"
#include "libslic3r/Utils.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_ObjectManipulation.hpp"
#include "GUI_Utils.hpp"
#include "wxExtensions.hpp"
#include "MainFrame.hpp"
#include "3DScene.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_Preview.hpp"
#include "Tab.hpp"
#include "PresetBundle.hpp"
#include "BackgroundSlicingProcess.hpp"
#include "ProgressStatusBar.hpp"
#include "PrintHostDialogs.hpp"
#include "../Utils/ASCIIFolding.hpp"
#include "../Utils/PrintHost.hpp"
#include "../Utils/FixModelByWin10.hpp"

#include <wx/glcanvas.h>    // Needs to be last because reasons :-/
#include "WipeTowerDialog.hpp"

using boost::optional;
namespace fs = boost::filesystem;
using Slic3r::_3DScene;
using Slic3r::Preset;
using Slic3r::PrintHostJob;


namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_SCHEDULE_BACKGROUND_PROCESS,     SimpleEvent);
wxDEFINE_EVENT(EVT_SLICING_UPDATE,                  SlicingStatusEvent);
wxDEFINE_EVENT(EVT_SLICING_COMPLETED,               wxCommandEvent);
wxDEFINE_EVENT(EVT_PROCESS_COMPLETED,               wxCommandEvent);

// Sidebar widgets

// struct InfoBox : public wxStaticBox
// {
//     InfoBox(wxWindow *parent, const wxString &label) :
//         wxStaticBox(parent, wxID_ANY, label)
//     {
//         SetFont(GUI::small_font().Bold());
//     }
// };

class ObjectInfo : public wxStaticBoxSizer
{
public:
    ObjectInfo(wxWindow *parent);

    wxStaticBitmap *manifold_warning_icon;
    wxStaticText *info_size;
    wxStaticText *info_volume;
    wxStaticText *info_facets;
    wxStaticText *info_materials;
    wxStaticText *info_manifold;
    bool        showing_manifold_warning_icon;
    void        show_sizer(bool show);
};

ObjectInfo::ObjectInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _(L("Info"))), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());

    auto *grid_sizer = new wxFlexGridSizer(4, 5, 5);
    grid_sizer->SetFlexibleDirection(wxHORIZONTAL);
    grid_sizer->AddGrowableCol(1, 1);
    grid_sizer->AddGrowableCol(3, 1);

    auto init_info_label = [parent, grid_sizer](wxStaticText **info_label, wxString text_label) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label+":");
        text->SetFont(wxGetApp().small_font());
        *info_label = new wxStaticText(parent, wxID_ANY, "");
        (*info_label)->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        grid_sizer->Add(*info_label, 0);
    };

    init_info_label(&info_size, _(L("Size")));
    init_info_label(&info_volume, _(L("Volume")));
    init_info_label(&info_facets, _(L("Facets")));
    init_info_label(&info_materials, _(L("Materials")));
    Add(grid_sizer, 0, wxEXPAND);

    auto *info_manifold_text = new wxStaticText(parent, wxID_ANY, _(L("Manifold")));
    info_manifold_text->SetFont(wxGetApp().small_font());
    info_manifold = new wxStaticText(parent, wxID_ANY, "");
    info_manifold->SetFont(wxGetApp().small_font());
    wxBitmap bitmap(GUI::from_u8(Slic3r::var("error.png")), wxBITMAP_TYPE_PNG);
    manifold_warning_icon = new wxStaticBitmap(parent, wxID_ANY, bitmap);
    auto *sizer_manifold = new wxBoxSizer(wxHORIZONTAL);
    sizer_manifold->Add(info_manifold_text, 0);
    sizer_manifold->Add(manifold_warning_icon, 0, wxLEFT, 2);
    sizer_manifold->Add(info_manifold, 0, wxLEFT, 2);
    Add(sizer_manifold, 0, wxEXPAND | wxTOP, 4);
}

void ObjectInfo::show_sizer(bool show)
{
    Show(show);
    if (show)
        manifold_warning_icon->Show(showing_manifold_warning_icon && show);
}

enum SlisedInfoIdx
{
    siFilament_m,
    siFilament_mm3,
    siFilament_g,
    siCost,
    siEstimatedTime,
    siWTNumbetOfToolchanges,

    siCount
};

class SlicedInfo : public wxStaticBoxSizer
{
public:
    SlicedInfo(wxWindow *parent);
    void SetTextAndShow(SlisedInfoIdx idx, const wxString& text, const wxString& new_label="");

private:
    std::vector<std::pair<wxStaticText*, wxStaticText*>> info_vec;
};

SlicedInfo::SlicedInfo(wxWindow *parent) :
    wxStaticBoxSizer(new wxStaticBox(parent, wxID_ANY, _(L("Sliced Info"))), wxVERTICAL)
{
    GetStaticBox()->SetFont(wxGetApp().bold_font());

    auto *grid_sizer = new wxFlexGridSizer(2, 5, 15);
    grid_sizer->SetFlexibleDirection(wxVERTICAL);

    info_vec.reserve(siCount);

    auto init_info_label = [this, parent, grid_sizer](wxString text_label) {
        auto *text = new wxStaticText(parent, wxID_ANY, text_label);
        text->SetFont(wxGetApp().small_font());
        auto info_label = new wxStaticText(parent, wxID_ANY, "N/A");
        info_label->SetFont(wxGetApp().small_font());
        grid_sizer->Add(text, 0);
        grid_sizer->Add(info_label, 0);
        info_vec.push_back(std::pair<wxStaticText*, wxStaticText*>(text, info_label));
    };

    init_info_label(_(L("Used Filament (m)")));
    init_info_label(_(L("Used Filament (mm³)")));
    init_info_label(_(L("Used Filament (g)")));
    init_info_label(_(L("Cost")));
    init_info_label(_(L("Estimated printing time")));
    init_info_label(_(L("Number of tool changes")));

    Add(grid_sizer, 0, wxEXPAND);
    this->Show(false);
}

void SlicedInfo::SetTextAndShow(SlisedInfoIdx idx, const wxString& text, const wxString& new_label/*=""*/)
{
    const bool show = text != "N/A";
    if (show)
        info_vec[idx].second->SetLabelText(text);
    if (!new_label.IsEmpty())
        info_vec[idx].first->SetLabelText(new_label);
    info_vec[idx].first->Show(show);
    info_vec[idx].second->Show(show);
}

PresetComboBox::PresetComboBox(wxWindow *parent, Preset::Type preset_type) :
    wxBitmapComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(200,-1), 0, nullptr, wxCB_READONLY),
    preset_type(preset_type),
    last_selected(wxNOT_FOUND)
{
    Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &evt) {
        auto selected_item = this->GetSelection();

        auto marker = reinterpret_cast<Marker>(this->GetClientData(selected_item));
        if (marker == LABEL_ITEM_MARKER) {
            this->SetSelection(this->last_selected);
            evt.StopPropagation();
        } else if ( this->last_selected != selected_item || 
                    wxGetApp().get_tab(this->preset_type)->get_presets()->current_is_dirty() ) {
            this->last_selected = selected_item;
            evt.SetInt(this->preset_type);
            evt.Skip();
        } else {
            evt.StopPropagation();
        }
    });
    
    if (preset_type == Slic3r::Preset::TYPE_FILAMENT)
    {
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
            if (extruder_idx < 0 || event.GetLogicalPosition(wxClientDC(this)).x > 24) {
                // Let the combo box process the mouse click.
                event.Skip();
                return;
            }
            
            // Swallow the mouse click and open the color picker.
            auto data = new wxColourData();
            data->SetChooseFull(1);
            auto dialog = new wxColourDialog(/* wxGetApp().mainframe */this, data);
            dialog->CenterOnParent();
            if (dialog->ShowModal() == wxID_OK) {
                DynamicPrintConfig cfg = *wxGetApp().get_tab(Preset::TYPE_PRINTER)->get_config(); 

                //FIXME this is too expensive to call full_config to get just the extruder color!
                auto colors = static_cast<ConfigOptionStrings*>(wxGetApp().preset_bundle->full_config().option("extruder_colour")->clone());
                colors->values[extruder_idx] = dialog->GetColourData().GetColour().GetAsString(wxC2S_HTML_SYNTAX);

                cfg.set_key_value("extruder_colour", colors);

                wxGetApp().get_tab(Preset::TYPE_PRINTER)->load_config(cfg);
                wxGetApp().preset_bundle->update_platter_filament_ui(extruder_idx, this);
                wxGetApp().plater()->on_config_change(cfg);
            }
            dialog->Destroy();
        });
    }
}

PresetComboBox::~PresetComboBox() {}


void PresetComboBox::set_label_marker(int item)
{
    this->SetClientData(item, (void*)LABEL_ITEM_MARKER);
}

void PresetComboBox::check_selection()
{
    if (this->last_selected != GetSelection())
        this->last_selected = GetSelection();
}

// Frequently changed parameters

class FreqChangedParams : public OG_Settings
{
    double		    m_brim_width = 0.0;
    wxButton*       m_wiping_dialog_button{ nullptr };
    wxSizer*        m_sizer {nullptr};

    std::shared_ptr<ConfigOptionsGroup> m_og_sla;
public:
    FreqChangedParams(wxWindow* parent, const int label_width);
    ~FreqChangedParams() {}

    wxButton*       get_wiping_dialog_button() { return m_wiping_dialog_button; }
    wxSizer*        get_sizer() override;
    ConfigOptionsGroup* get_og(const bool is_fff);
    void            Show(const bool is_fff);
};

FreqChangedParams::FreqChangedParams(wxWindow* parent, const int label_width) :
    OG_Settings(parent, false)
{
    DynamicPrintConfig*	config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;

    // Frequently changed parameters for FFF_technology
    m_og->set_config(config);
    m_og->label_width = label_width;

    m_og->m_on_change = [config, this](t_config_option_key opt_key, boost::any value) {
        Tab* tab_print = wxGetApp().get_tab(Preset::TYPE_PRINT);
        if (!tab_print) return;

        if (opt_key == "fill_density") {
            value = m_og->get_config_value(*config, opt_key);
            tab_print->set_value(opt_key, value);
            tab_print->update();
        }
        else{
            DynamicPrintConfig new_conf = *config;
            if (opt_key == "brim") {
                double new_val;
                double brim_width = config->opt_float("brim_width");
                if (boost::any_cast<bool>(value) == true)
                {
                    new_val = m_brim_width == 0.0 ? 5 :
                        m_brim_width < 0.0 ? m_brim_width * (-1) :
                        m_brim_width;
                }
                else {
                    m_brim_width = brim_width * (-1);
                    new_val = 0;
                }
                new_conf.set_key_value("brim_width", new ConfigOptionFloat(new_val));
            }
            else { //(opt_key == "support")
                const wxString& selection = boost::any_cast<wxString>(value);

                auto support_material = selection == _("None") ? false : true;
                new_conf.set_key_value("support_material", new ConfigOptionBool(support_material));

                if (selection == _("Everywhere"))
                    new_conf.set_key_value("support_material_buildplate_only", new ConfigOptionBool(false));
                else if (selection == _("Support on build plate only"))
                    new_conf.set_key_value("support_material_buildplate_only", new ConfigOptionBool(true));
            }
            tab_print->load_config(new_conf);
        }

        tab_print->update_dirty();
    };

    Option option = m_og->get_option("fill_density");
    option.opt.sidetext = "";
    option.opt.full_width = true;
    m_og->append_single_option_line(option);

    ConfigOptionDef def;

    def.label = L("Support");
    def.type = coStrings;
    def.gui_type = "select_open";
    def.tooltip = L("Select what kind of support do you need");
    def.enum_labels.push_back(L("None"));
    def.enum_labels.push_back(L("Support on build plate only"));
    def.enum_labels.push_back(L("Everywhere"));
    std::string selection = !config->opt_bool("support_material") ?
        "None" :
        config->opt_bool("support_material_buildplate_only") ?
        "Support on build plate only" :
        "Everywhere";
    def.default_value = new ConfigOptionStrings{ selection };
    option = Option(def, "support");
    option.opt.full_width = true;
    m_og->append_single_option_line(option);

    m_brim_width = config->opt_float("brim_width");
    def.label = L("Brim");
    def.type = coBool;
    def.tooltip = L("This flag enables the brim that will be printed around each object on the first layer.");
    def.gui_type = "";
    def.default_value = new ConfigOptionBool{ m_brim_width > 0.0 ? true : false };
    option = Option(def, "brim");
    m_og->append_single_option_line(option);


    Line line = { "", "" };
    line.widget = [config, this](wxWindow* parent) {
        m_wiping_dialog_button = new wxButton(parent, wxID_ANY, _(L("Purging volumes")) + dots, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        auto sizer = new wxBoxSizer(wxHORIZONTAL);
        sizer->Add(m_wiping_dialog_button);
        m_wiping_dialog_button->Bind(wxEVT_BUTTON, ([parent](wxCommandEvent& e)
        {
            auto &config = wxGetApp().preset_bundle->project_config;
            const std::vector<double> &init_matrix = (config.option<ConfigOptionFloats>("wiping_volumes_matrix"))->values;
            const std::vector<double> &init_extruders = (config.option<ConfigOptionFloats>("wiping_volumes_extruders"))->values;

            WipingDialog dlg(parent, cast<float>(init_matrix), cast<float>(init_extruders));

            if (dlg.ShowModal() == wxID_OK) {
                std::vector<float> matrix = dlg.get_matrix();
                std::vector<float> extruders = dlg.get_extruders();
                (config.option<ConfigOptionFloats>("wiping_volumes_matrix"))->values = std::vector<double>(matrix.begin(), matrix.end());
                (config.option<ConfigOptionFloats>("wiping_volumes_extruders"))->values = std::vector<double>(extruders.begin(), extruders.end());
				wxPostEvent(parent, SimpleEvent(EVT_SCHEDULE_BACKGROUND_PROCESS, parent));
            }
        }));
        return sizer;
    };
    m_og->append_line(line);


    // Frequently changed parameters for SLA_technology
    m_og_sla = std::make_shared<ConfigOptionsGroup>(parent, "");
    DynamicPrintConfig*	config_sla = &wxGetApp().preset_bundle->sla_prints.get_edited_preset().config;
    m_og_sla->set_config(config_sla);
    m_og_sla->label_width = label_width*2;

    m_og_sla->m_on_change = [config_sla, this](t_config_option_key opt_key, boost::any value) {
        Tab* tab = wxGetApp().get_tab(Preset::TYPE_SLA_PRINT);
        if (!tab) return;

        tab->set_value(opt_key, value);

        DynamicPrintConfig new_conf = *config_sla;
        new_conf.set_key_value(opt_key, new ConfigOptionBool(boost::any_cast<bool>(value)));
        tab->load_config(new_conf);
        tab->update_dirty();
    };    

    m_og_sla->append_single_option_line("supports_enable");
    m_og_sla->append_single_option_line("pad_enable");

    m_sizer = new wxBoxSizer(wxVERTICAL);
    m_sizer->Add(m_og->sizer, 0, wxEXPAND);
    m_sizer->Add(m_og_sla->sizer, 0, wxEXPAND | wxTOP, 5);
}


wxSizer* FreqChangedParams::get_sizer()
{
    return m_sizer;
}

void FreqChangedParams::Show(const bool is_fff)
{
    const bool is_wdb_shown = m_wiping_dialog_button->IsShown();
    m_og->Show(is_fff);
    m_og_sla->Show(!is_fff);

    // correct showing of the FreqChangedParams sizer when m_wiping_dialog_button is hidden 
    if (is_fff && !is_wdb_shown)
        m_wiping_dialog_button->Hide();
}

ConfigOptionsGroup* FreqChangedParams::get_og(const bool is_fff)
{
    return is_fff ? m_og.get() : m_og_sla.get();
}

// Sidebar / private

struct Sidebar::priv
{
    Plater *plater;

    wxScrolledWindow *scrolled;

    PrusaModeSizer  *mode_sizer;
    wxFlexGridSizer *sizer_presets;
    PresetComboBox *combo_print;
    std::vector<PresetComboBox*> combos_filament;
    wxBoxSizer *sizer_filaments;
    PresetComboBox *combo_sla_print;
    PresetComboBox *combo_sla_material;
    PresetComboBox *combo_printer;

    wxBoxSizer *sizer_params;
    FreqChangedParams   *frequently_changed_parameters;
    ObjectList          *object_list;
    ObjectManipulation  *object_manipulation;
    ObjectSettings      *object_settings;
    ObjectInfo *object_info;
    SlicedInfo *sliced_info;

    wxButton *btn_export_gcode;
    wxButton *btn_reslice;
    wxButton *btn_send_gcode;

    priv(Plater *plater) : plater(plater) {}

    void show_preset_comboboxes();
};

void Sidebar::priv::show_preset_comboboxes()
{
    const bool showSLA = plater->printer_technology() == ptSLA;

    wxWindowUpdateLocker noUpdates_scrolled(scrolled->GetParent());
    
    for (size_t i = 0; i < 4; ++i)
        sizer_presets->Show(i, !showSLA);

    for (size_t i = 4; i < 8; ++i) {
        if (sizer_presets->IsShown(i) != showSLA)
            sizer_presets->Show(i, showSLA);
    }

    frequently_changed_parameters->Show(!showSLA);

    scrolled->GetParent()->Layout();
    scrolled->Refresh();
}


// Sidebar / public

Sidebar::Sidebar(Plater *parent)
    : wxPanel(parent), p(new priv(parent))
{
    p->scrolled = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(400, -1));
    p->scrolled->SetScrollbars(0, 20, 1, 2);

    // Sizer in the scrolled area
    auto *scrolled_sizer = new wxBoxSizer(wxVERTICAL);
    p->scrolled->SetSizer(scrolled_sizer);

    // Sizer with buttons for mode changing
    p->mode_sizer = new PrusaModeSizer(p->scrolled);

    // The preset chooser
    p->sizer_presets = new wxFlexGridSizer(5, 2, 1, 2);
    p->sizer_presets->AddGrowableCol(1, 1);
    p->sizer_presets->SetFlexibleDirection(wxBOTH);
    p->sizer_filaments = new wxBoxSizer(wxVERTICAL);

    auto init_combo = [this](PresetComboBox **combo, wxString label, Preset::Type preset_type, bool filament) {
        auto *text = new wxStaticText(p->scrolled, wxID_ANY, label);
        text->SetFont(wxGetApp().small_font());
        *combo = new PresetComboBox(p->scrolled, preset_type);

        auto *sizer_presets = this->p->sizer_presets;
        auto *sizer_filaments = this->p->sizer_filaments;
        sizer_presets->Add(text, 0, wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL | wxRIGHT, 4);
        if (! filament) {
            sizer_presets->Add(*combo, 1, wxALIGN_CENTER_VERTICAL | wxEXPAND | wxBOTTOM, 1);
        } else {
            sizer_filaments->Add(*combo, 1, wxEXPAND | wxBOTTOM, 1);
            (*combo)->set_extruder_idx(0);
            sizer_presets->Add(sizer_filaments, 1, wxEXPAND);
        }
    };

    p->combos_filament.push_back(nullptr);
    init_combo(&p->combo_print,         _(L("Print settings")), Preset::TYPE_PRINT,         false);
    init_combo(&p->combos_filament[0],  _(L("Filament")),       Preset::TYPE_FILAMENT,      true);
    init_combo(&p->combo_sla_print,     _(L("SLA print")),      Preset::TYPE_SLA_PRINT,     false);
    init_combo(&p->combo_sla_material,  _(L("SLA material")),   Preset::TYPE_SLA_MATERIAL,  false);
    init_combo(&p->combo_printer,       _(L("Printer")),        Preset::TYPE_PRINTER,       false);

    // calculate width of the preset labels 
    p->sizer_presets->Layout();
    const wxArrayInt& ar = p->sizer_presets->GetColWidths();
    int label_width = ar.IsEmpty() ? 100 : ar.front()-4;

    p->sizer_params = new wxBoxSizer(wxVERTICAL);

    // Frequently changed parameters
    p->frequently_changed_parameters = new FreqChangedParams(p->scrolled, label_width);
    p->sizer_params->Add(p->frequently_changed_parameters->get_sizer(), 0, wxEXPAND | wxBOTTOM | wxLEFT, 2);
    
    // Object List
    p->object_list = new ObjectList(p->scrolled);
    p->sizer_params->Add(p->object_list->get_sizer(), 1, wxEXPAND | wxTOP, 20);
 
    // Object Manipulations
    p->object_manipulation = new ObjectManipulation(p->scrolled);
    p->object_manipulation->Hide();
    p->sizer_params->Add(p->object_manipulation->get_sizer(), 0, wxEXPAND | wxLEFT | wxTOP, 20);

    // Frequently Object Settings
    p->object_settings = new ObjectSettings(p->scrolled);
    p->object_settings->Hide();
    p->sizer_params->Add(p->object_settings->get_sizer(), 0, wxEXPAND | wxLEFT | wxTOP, 20);

    wxBitmap arrow_up(GUI::from_u8(Slic3r::var("brick_go.png")), wxBITMAP_TYPE_PNG);
    p->btn_send_gcode = new wxButton(this, wxID_ANY, _(L("Send to printer")));
    p->btn_send_gcode->SetBitmap(arrow_up);
    p->btn_send_gcode->SetFont(wxGetApp().bold_font());
    p->btn_send_gcode->Hide();

    // Info boxes
    p->object_info = new ObjectInfo(p->scrolled);
    p->sliced_info = new SlicedInfo(p->scrolled);

    // Sizer in the scrolled area
    scrolled_sizer->Add(p->mode_sizer, 0, wxALIGN_RIGHT/*CENTER_HORIZONTAL*/ | wxBOTTOM | wxRIGHT, 5);
    scrolled_sizer->Add(p->sizer_presets, 0, wxEXPAND | wxLEFT, 2);
    scrolled_sizer->Add(p->sizer_params, 1, wxEXPAND);
    scrolled_sizer->Add(p->object_info, 0, wxEXPAND | wxTOP | wxLEFT, 20);
    scrolled_sizer->Add(p->sliced_info, 0, wxEXPAND | wxTOP | wxLEFT, 20);

    // Buttons underneath the scrolled area
    p->btn_export_gcode = new wxButton(this, wxID_ANY, _(L("Export G-code")) + dots);
    p->btn_export_gcode->SetFont(wxGetApp().bold_font());
    p->btn_reslice = new wxButton(this, wxID_ANY, _(L("Slice now")));
    p->btn_reslice->SetFont(wxGetApp().bold_font());
    enable_buttons(false);

    auto *btns_sizer = new wxBoxSizer(wxVERTICAL);
    btns_sizer->Add(p->btn_reslice, 0, wxEXPAND | wxTOP, 5);
    btns_sizer->Add(p->btn_send_gcode, 0, wxEXPAND | wxTOP, 5);
    btns_sizer->Add(p->btn_export_gcode, 0, wxEXPAND | wxTOP, 5);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(p->scrolled, 1, wxEXPAND | wxTOP, 5);
    sizer->Add(btns_sizer, 0, wxEXPAND | wxLEFT, 20);
    SetSizer(sizer);

    // Events
    p->btn_export_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { p->plater->export_gcode(); });
    p->btn_reslice->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { p->plater->reslice(); });
    p->btn_send_gcode->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { p->plater->send_gcode(); });
}

Sidebar::~Sidebar() {}

void Sidebar::init_filament_combo(PresetComboBox **combo, const int extr_idx) {
    *combo = new PresetComboBox(p->scrolled, Slic3r::Preset::TYPE_FILAMENT);
//         # copy icons from first choice
//         $choice->SetItemBitmap($_, $choices->[0]->GetItemBitmap($_)) for 0..$#presets;

    (*combo)->set_extruder_idx(extr_idx);

    auto /***/sizer_filaments = this->p->sizer_filaments;
    sizer_filaments->Add(*combo, 1, wxEXPAND | wxBOTTOM, 1);
}

void Sidebar::remove_unused_filament_combos(const int current_extruder_count)
{
    if (current_extruder_count >= p->combos_filament.size())
        return;
    auto sizer_filaments = this->p->sizer_filaments;
    while (p->combos_filament.size() > current_extruder_count) {
        const int last = p->combos_filament.size() - 1;
        sizer_filaments->Remove(last);
        (*p->combos_filament[last]).Destroy();
        p->combos_filament.pop_back();
    }
}

void Sidebar::update_presets(Preset::Type preset_type)
{
	PresetBundle &preset_bundle = *wxGetApp().preset_bundle;

    switch (preset_type) {
    case Preset::TYPE_FILAMENT: 
    {
        const int extruder_cnt = p->plater->printer_technology() != ptFFF ? 1 :
                                dynamic_cast<ConfigOptionFloats*>(preset_bundle.printers.get_edited_preset().config.option("nozzle_diameter"))->values.size();
        const int filament_cnt = p->combos_filament.size() > extruder_cnt ? extruder_cnt : p->combos_filament.size();

        if (filament_cnt == 1) {
            // Single filament printer, synchronize the filament presets.
            const std::string &name = preset_bundle.filaments.get_selected_preset().name;
            preset_bundle.set_filament_preset(0, name);
        }

        for (size_t i = 0; i < filament_cnt; i++) {
            preset_bundle.update_platter_filament_ui(i, p->combos_filament[i]);
        }

        break;
    }

    case Preset::TYPE_PRINT:
		preset_bundle.prints.update_platter_ui(p->combo_print);
        break;

    case Preset::TYPE_SLA_PRINT:
		preset_bundle.sla_prints.update_platter_ui(p->combo_sla_print);
        break;

    case Preset::TYPE_SLA_MATERIAL:
		preset_bundle.sla_materials.update_platter_ui(p->combo_sla_material);
        break;

	case Preset::TYPE_PRINTER:
	{
		// Update the print choosers to only contain the compatible presets, update the dirty flags.
		if (p->plater->printer_technology() == ptFFF)
			preset_bundle.prints.update_platter_ui(p->combo_print);
        else {
            preset_bundle.sla_prints.update_platter_ui(p->combo_sla_print);
            preset_bundle.sla_materials.update_platter_ui(p->combo_sla_material);
        }
		// Update the printer choosers, update the dirty flags.
        auto prev_selection = p->combo_printer->GetSelection();
		preset_bundle.printers.update_platter_ui(p->combo_printer);
        if (prev_selection != p->combo_printer->GetSelection())
            p->combo_printer->check_selection();
		// Update the filament choosers to only contain the compatible presets, update the color preview,
		// update the dirty flags.
		if (p->plater->printer_technology() == ptFFF) {
            for (size_t i = 0; i < p->combos_filament.size(); ++ i)
                preset_bundle.update_platter_filament_ui(i, p->combos_filament[i]);
		}
		p->show_preset_comboboxes();
		break;
	}

    default: break;
    }

    // Synchronize config.ini with the current selections.
    wxGetApp().preset_bundle->export_selections(*wxGetApp().app_config);
}

void Sidebar::update_mode_sizer(const Slic3r::ConfigOptionMode& mode)
{
    p->mode_sizer->SetMode(mode);
}

ObjectManipulation* Sidebar::obj_manipul()
{
    return p->object_manipulation;
}

ObjectList* Sidebar::obj_list()
{
    return p->object_list;
}

ObjectSettings* Sidebar::obj_settings()
{
    return p->object_settings;
}

wxScrolledWindow* Sidebar::scrolled_panel()
{
    return p->scrolled;
}

ConfigOptionsGroup* Sidebar::og_freq_chng_params(const bool is_fff)
{
    return p->frequently_changed_parameters->get_og(is_fff);
}

wxButton* Sidebar::get_wiping_dialog_button()
{
    return p->frequently_changed_parameters->get_wiping_dialog_button();
}

void Sidebar::update_objects_list_extruder_column(int extruders_count)
{
    p->object_list->update_objects_list_extruder_column(extruders_count);
}

void Sidebar::show_info_sizer()
{
    if (!p->plater->is_single_full_object_selection() ||
        m_mode < comExpert ||
        p->plater->model().objects.empty()) {
        p->object_info->Show(false);
        return;
    }

    int obj_idx = p->plater->get_selected_object_idx();

    const ModelObject* model_object = p->plater->model().objects[obj_idx];
    // hack to avoid crash when deleting the last object on the bed
    if (model_object->volumes.empty())
    {
        p->object_info->Show(false);
        return;
    }

    const ModelInstance* model_instance = !model_object->instances.empty() ? model_object->instances.front() : nullptr;

    auto size = model_object->bounding_box().size();
    p->object_info->info_size->SetLabel(wxString::Format("%.2f x %.2f x %.2f",size(0), size(1), size(2)));
    p->object_info->info_materials->SetLabel(wxString::Format("%d", static_cast<int>(model_object->materials_count())));

    auto& stats = model_object->volumes.front()->mesh.stl.stats;
    p->object_info->info_volume->SetLabel(wxString::Format("%.2f", size(0) * size(1) * size(2)));
    p->object_info->info_facets->SetLabel(wxString::Format(_(L("%d (%d shells)")), static_cast<int>(model_object->facets_count()), stats.number_of_parts));

    int errors = stats.degenerate_facets + stats.edges_fixed + stats.facets_removed +
        stats.facets_added + stats.facets_reversed + stats.backwards_edges;
    if (errors > 0) {
        wxString tooltip = wxString::Format(_(L("Auto-repaired (%d errors)")), errors);
        p->object_info->info_manifold->SetLabel(tooltip);
        
        tooltip += ":\n" + wxString::Format(_(L("%d degenerate facets, %d edges fixed, %d facets removed, "
                                        "%d facets added, %d facets reversed, %d backwards edges")),
                                        stats.degenerate_facets, stats.edges_fixed, stats.facets_removed,
                                        stats.facets_added, stats.facets_reversed, stats.backwards_edges);

        p->object_info->showing_manifold_warning_icon = true;
        p->object_info->info_manifold->SetToolTip(tooltip);
        p->object_info->manifold_warning_icon->SetToolTip(tooltip);
    } 
    else {
        p->object_info->info_manifold->SetLabel(L("Yes"));
        p->object_info->showing_manifold_warning_icon = false;
        p->object_info->info_manifold->SetToolTip("");
        p->object_info->manifold_warning_icon->SetToolTip("");
    }

    p->object_info->show_sizer(true);
}

void Sidebar::show_sliced_info_sizer(const bool show) 
{
    wxWindowUpdateLocker freeze_guard(this);

    p->sliced_info->Show(show);
    if (show) {
        const PrintStatistics& ps = p->plater->fff_print().print_statistics();
        const bool is_wipe_tower = ps.total_wipe_tower_filament > 0;

        wxString new_label = _(L("Used Filament (m)"));
        if (is_wipe_tower)
            new_label += wxString::Format(" :\n    - %s\n    - %s", _(L("objects")), _(L("wipe tower")));

        wxString info_text = is_wipe_tower ?
                            wxString::Format("%.2f \n%.2f \n%.2f", ps.total_used_filament / 1000,
                                            (ps.total_used_filament - ps.total_wipe_tower_filament) / 1000, 
                                            ps.total_wipe_tower_filament / 1000) :
                            wxString::Format("%.2f", ps.total_used_filament / 1000);
        p->sliced_info->SetTextAndShow(siFilament_m,    info_text,      new_label);

        p->sliced_info->SetTextAndShow(siFilament_mm3,  wxString::Format("%.2f", ps.total_extruded_volume));
        p->sliced_info->SetTextAndShow(siFilament_g,    wxString::Format("%.2f", ps.total_weight));


        new_label = _(L("Cost"));
        if (is_wipe_tower)
            new_label += wxString::Format(" :\n    - %s\n    - %s", _(L("objects")), _(L("wipe tower")));
                     
        info_text = is_wipe_tower ?
                    wxString::Format("%.2f \n%.2f \n%.2f", ps.total_cost,
                                        (ps.total_cost - ps.total_wipe_tower_cost), 
                                        ps.total_wipe_tower_cost) :
                    wxString::Format("%.2f", ps.total_cost);
        p->sliced_info->SetTextAndShow(siCost,       info_text,      new_label);

        if (ps.estimated_normal_print_time == "N/A" && ps.estimated_silent_print_time == "N/A")
            p->sliced_info->SetTextAndShow(siEstimatedTime, "N/A");
        else {
            new_label = _(L("Estimated printing time")) +" :";
            info_text = "";
            if (ps.estimated_normal_print_time != "N/A") {
                new_label += wxString::Format("\n    - %s", _(L("normal mode")));
                info_text += wxString::Format("\n%s", ps.estimated_normal_print_time);
            }
            if (ps.estimated_silent_print_time != "N/A") {
                new_label += wxString::Format("\n    - %s", _(L("silent mode")));
                info_text += wxString::Format("\n%s", ps.estimated_silent_print_time);
            }
            p->sliced_info->SetTextAndShow(siEstimatedTime,  info_text,      new_label);
        }

        // if there is a wipe tower, insert number of toolchanges info into the array:
        p->sliced_info->SetTextAndShow(siWTNumbetOfToolchanges, is_wipe_tower ? wxString::Format("%.d", p->plater->fff_print().wipe_tower_data().number_of_toolchanges) : "N/A");
    }

    Layout();
    p->scrolled->Refresh();
}

void Sidebar::enable_buttons(bool enable)
{
    p->btn_reslice->Enable(enable);
    p->btn_export_gcode->Enable(enable);
    p->btn_send_gcode->Enable(enable);
}

void Sidebar::show_reslice(bool show) { p->btn_reslice->Show(show); }
void Sidebar::show_send(bool show) { p->btn_send_gcode->Show(show); }

bool Sidebar::is_multifilament()
{
    return p->combos_filament.size() > 1;
}


std::vector<PresetComboBox*>& Sidebar::combos_filament()
{
    return p->combos_filament;
}

// Plater::DropTarget

class PlaterDropTarget : public wxFileDropTarget
{
public:
	PlaterDropTarget(Plater *plater) : plater(plater) { this->SetDefaultAction(wxDragCopy); }

    virtual bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames);

private:
    Plater *plater;

    static const std::regex pattern_drop;
};

const std::regex PlaterDropTarget::pattern_drop(".*[.](stl|obj|amf|3mf|prusa)", std::regex::icase);

bool PlaterDropTarget::OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &filenames)
{
    std::vector<fs::path> paths;

    for (const auto &filename : filenames) {
        fs::path path(into_path(filename));

        if (std::regex_match(path.string(), pattern_drop)) {
            paths.push_back(std::move(path));
        } else {
            return false;
        }
    }

    plater->load_files(paths);
    return true;
}

// Plater / private
struct Plater::priv
{
    // PIMPL back pointer ("Q-Pointer")
    Plater *q;
    MainFrame *main_frame;

    // Object popup menu
    PrusaMenu object_menu;
    // Part popup menu
    PrusaMenu part_menu;
    // SLA-Object popup menu
    PrusaMenu sla_object_menu;

    // Data
    Slic3r::DynamicPrintConfig *config;        // FIXME: leak?
    Slic3r::Print               fff_print;
	Slic3r::SLAPrint            sla_print;
    Slic3r::Model               model;
    PrinterTechnology           printer_technology = ptFFF;
    Slic3r::GCodePreviewData    gcode_preview_data;

    // GUI elements
    wxSizer* panel_sizer;
    wxPanel* current_panel;
    std::vector<wxPanel*> panels;
    Sidebar *sidebar;
    View3D* view3D;
    GLToolbar view_toolbar;
    Preview *preview;

    wxString project_filename;

    BackgroundSlicingProcess    background_process;
    std::atomic<bool>           arranging;
    std::atomic<bool>           rotoptimizing;
    bool                        delayed_scene_refresh;

    wxTimer                     background_process_timer;

    static const std::regex pattern_bundle;
    static const std::regex pattern_3mf;
    static const std::regex pattern_zip_amf;
    static const std::regex pattern_any_amf;
#if ENABLE_VOLUMES_CENTERING_FIXES
    static const std::regex pattern_prusa;
#endif // ENABLE_VOLUMES_CENTERING_FIXES

    priv(Plater *q, MainFrame *main_frame);

    void update(bool force_full_scene_refresh = false);
    void select_view(const std::string& direction);
    void select_view_3D(const std::string& name);
    void select_next_view_3D();
    void update_ui_from_settings();
    ProgressStatusBar* statusbar();
    std::string get_config(const std::string &key) const;
    BoundingBoxf bed_shape_bb() const;
    BoundingBox scaled_bed_shape_bb() const;
    std::vector<size_t> load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config);
    std::vector<size_t> load_model_objects(const ModelObjectPtrs &model_objects);
    std::unique_ptr<CheckboxFileDialog> get_export_file(GUI::FileType file_type);

    const GLCanvas3D::Selection& get_selection() const;
    GLCanvas3D::Selection& get_selection();
    int get_selected_object_idx() const;
    int get_selected_volume_idx() const;
    void selection_changed();
    void object_list_changed();

    void select_all();
    void remove(size_t obj_idx);
    void delete_object_from_model(size_t obj_idx);
    void reset();
    void mirror(Axis axis);
    void arrange();
    void sla_optimize_rotation();
    void split_object();
    void split_volume();
	bool background_processing_enabled() const { return this->get_config("background_processing") == "1"; }
    void update_print_volume_state();
    void schedule_background_process();
    // Update background processing thread from the current config and Model.
    enum UpdateBackgroundProcessReturnState {
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that the background process was invalidated and it needs to be re-run.
        UPDATE_BACKGROUND_PROCESS_RESTART = 1,
        // update_background_process() reports, that the Print / SLAPrint was updated in a way,
        // that a scene needs to be refreshed (you should call _3DScene::reload_scene(canvas3Dwidget, false))
        UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE = 2,   
        // update_background_process() reports, that the Print / SLAPrint is invalid, and the error message
        // was sent to the status line.
        UPDATE_BACKGROUND_PROCESS_INVALID = 4,
        // Restart even if the background processing is disabled.
        UPDATE_BACKGROUND_PROCESS_FORCE_RESTART = 8,
        // Restart for G-code (or SLA zip) export or upload.
        UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT = 16,
    };
    // returns bit mask of UpdateBackgroundProcessReturnState
    unsigned int update_background_process(bool force_validation = false);
    // Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
    bool restart_background_process(unsigned int state);
	// returns bit mask of UpdateBackgroundProcessReturnState
	unsigned int update_restart_background_process(bool force_scene_update, bool force_preview_update);
    void export_gcode(fs::path output_path, PrintHostJob upload_job);
    void reload_from_disk();
    void fix_through_netfabb(const int obj_idx, const int vol_idx = -1);

    void set_current_panel(wxPanel* panel);

    void on_select_preset(wxCommandEvent&);
    void on_slicing_update(SlicingStatusEvent&);
    void on_slicing_completed(wxCommandEvent&);
    void on_process_completed(wxCommandEvent&);
    void on_layer_editing_toggled(bool enable);

    void on_action_add(SimpleEvent&);
    void on_action_split_objects(SimpleEvent&);
    void on_action_split_volumes(SimpleEvent&);
    void on_action_layersediting(SimpleEvent&);

    void on_object_select(SimpleEvent&);
    void on_viewport_changed(SimpleEvent&);
    void on_right_click(Vec2dEvent&);
    void on_wipetower_moved(Vec3dEvent&);
    void on_update_geometry(Vec3dsEvent<2>&);
    void on_3dcanvas_mouse_dragging_finished(SimpleEvent&);

    void update_object_menu();

private:
    bool init_object_menu();
    bool init_common_menu(wxMenu* menu, const bool is_part = false);
    bool complit_init_object_menu();
    bool complit_init_sla_object_menu();
    bool complit_init_part_menu();
    void init_view_toolbar();

    bool can_delete_object() const;
    bool can_increase_instances() const;
    bool can_decrease_instances() const;
    bool can_set_instance_to_object() const;
    bool can_split_to_objects() const;
    bool can_split_to_volumes() const;
    bool can_split() const;
    bool layers_height_allowed() const;
    bool can_delete_all() const;
    bool can_arrange() const;
    bool can_mirror() const;

    void update_fff_scene();
    void update_sla_scene();
};

const std::regex Plater::priv::pattern_bundle(".*[.](amf|amf[.]xml|zip[.]amf|3mf|prusa)", std::regex::icase);
const std::regex Plater::priv::pattern_3mf(".*3mf", std::regex::icase);
const std::regex Plater::priv::pattern_zip_amf(".*[.]zip[.]amf", std::regex::icase);
const std::regex Plater::priv::pattern_any_amf(".*[.](amf|amf[.]xml|zip[.]amf)", std::regex::icase);
#if ENABLE_VOLUMES_CENTERING_FIXES
const std::regex Plater::priv::pattern_prusa(".*prusa", std::regex::icase);
#endif // ENABLE_VOLUMES_CENTERING_FIXES

Plater::priv::priv(Plater *q, MainFrame *main_frame)
    : q(q)
    , main_frame(main_frame)
    , config(Slic3r::DynamicPrintConfig::new_from_defaults_keys({
        "bed_shape", "complete_objects", "extruder_clearance_radius", "skirts", "skirt_distance",
        "brim_width", "variable_layer_height", "serial_port", "serial_speed", "host_type", "print_host",
        "printhost_apikey", "printhost_cafile", "nozzle_diameter", "single_extruder_multi_material",
        "wipe_tower", "wipe_tower_x", "wipe_tower_y", "wipe_tower_width", "wipe_tower_rotation_angle",
        "extruder_colour", "filament_colour", "max_print_height", "printer_model", "printer_technology",
        // These values are necessary to construct SlicingParameters by the Canvas3D variable layer height editor.
        "layer_height", "first_layer_height", "min_layer_height", "max_layer_height",
        "brim_width", "perimeters", "perimeter_extruder", "fill_density", "infill_extruder", "top_solid_layers", "bottom_solid_layers", "solid_infill_extruder",
        "support_material", "support_material_extruder", "support_material_interface_extruder", "support_material_contact_distance", "raft_layers"
        }))
    , sidebar(new Sidebar(q))
    , delayed_scene_refresh(false)
    , project_filename(wxEmptyString)
    , view_toolbar(GLToolbar::Radio)
{
    arranging.store(false);
    rotoptimizing.store(false);
    background_process.set_fff_print(&fff_print);
	background_process.set_sla_print(&sla_print);
    background_process.set_gcode_preview_data(&gcode_preview_data);
    background_process.set_slicing_completed_event(EVT_SLICING_COMPLETED);
    background_process.set_finished_event(EVT_PROCESS_COMPLETED);
	// Default printer technology for default config.
    background_process.select_technology(this->printer_technology);
    // Register progress callback from the Print class to the Platter.

    auto statuscb = [this](const Slic3r::PrintBase::SlicingStatus &status) {
        wxQueueEvent(this->q, new Slic3r::SlicingStatusEvent(EVT_SLICING_UPDATE, 0, status));
    };
    fff_print.set_status_callback(statuscb);
    sla_print.set_status_callback(statuscb);
    this->q->Bind(EVT_SLICING_UPDATE, &priv::on_slicing_update, this);

    view3D = new View3D(q, &model, config, &background_process);
    preview = new Preview(q, config, &background_process, &gcode_preview_data, [this](){ schedule_background_process(); });
    // Let the Tab key switch between the 3D view and the layer preview.
    view3D->Bind(wxEVT_NAVIGATION_KEY, [this](wxNavigationKeyEvent &evt) { if (evt.IsFromTab()) this->select_next_view_3D(); });
    preview->Bind(wxEVT_NAVIGATION_KEY, [this](wxNavigationKeyEvent &evt) { if (evt.IsFromTab()) this->select_next_view_3D(); });

    panels.push_back(view3D);
    panels.push_back(preview);

    this->background_process_timer.SetOwner(this->q, 0);
    this->q->Bind(wxEVT_TIMER, [this](wxTimerEvent &evt) { this->update_restart_background_process(false, false); });

#if !ENABLE_REWORKED_BED_SHAPE_CHANGE
    auto *bed_shape = config->opt<ConfigOptionPoints>("bed_shape");
    view3D->set_bed_shape(bed_shape->values);
    preview->set_bed_shape(bed_shape->values);
#endif // !ENABLE_REWORKED_BED_SHAPE_CHANGE

    update();

    auto *hsizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer = new wxBoxSizer(wxHORIZONTAL);
    panel_sizer->Add(view3D, 1, wxEXPAND | wxALL, 0);
    panel_sizer->Add(preview, 1, wxEXPAND | wxALL, 0);
    hsizer->Add(panel_sizer, 1, wxEXPAND | wxALL, 0);
    hsizer->Add(sidebar, 0, wxEXPAND | wxLEFT | wxRIGHT, 0);
    q->SetSizer(hsizer);

    init_object_menu();

    // Events:

    // Preset change event
    sidebar->Bind(wxEVT_COMBOBOX, &priv::on_select_preset, this);

    sidebar->Bind(EVT_OBJ_LIST_OBJECT_SELECT, [this](wxEvent&) { priv::selection_changed(); });
    sidebar->Bind(EVT_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { this->schedule_background_process(); });

    wxGLCanvas* view3D_canvas = view3D->get_wxglcanvas();
    // 3DScene events:
    view3D_canvas->Bind(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS, [this](SimpleEvent&) { this->schedule_background_process(); });
    view3D_canvas->Bind(EVT_GLCANVAS_OBJECT_SELECT, &priv::on_object_select, this);
    view3D_canvas->Bind(EVT_GLCANVAS_VIEWPORT_CHANGED, &priv::on_viewport_changed, this);
    view3D_canvas->Bind(EVT_GLCANVAS_RIGHT_CLICK, &priv::on_right_click, this);
    view3D_canvas->Bind(EVT_GLCANVAS_REMOVE_OBJECT, [q](SimpleEvent&) { q->remove_selected(); });
    view3D_canvas->Bind(EVT_GLCANVAS_ARRANGE, [this](SimpleEvent&) { arrange(); });
    view3D_canvas->Bind(EVT_GLCANVAS_SELECT_ALL, [this](SimpleEvent&) { this->q->select_all(); });
    view3D_canvas->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INCREASE_INSTANCES, [this](Event<int> &evt) 
        { if (evt.data == 1) this->q->increase_instances(); else if (this->can_decrease_instances()) this->q->decrease_instances(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_MOVED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_WIPETOWER_MOVED, &priv::on_wipetower_moved, this);
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_ROTATED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_INSTANCE_SCALED, [this](SimpleEvent&) { update(); });
    view3D_canvas->Bind(EVT_GLCANVAS_ENABLE_ACTION_BUTTONS, [this](Event<bool> &evt) { this->sidebar->enable_buttons(evt.data); });
    view3D_canvas->Bind(EVT_GLCANVAS_UPDATE_GEOMETRY, &priv::on_update_geometry, this);
    view3D_canvas->Bind(EVT_GLCANVAS_MOUSE_DRAGGING_FINISHED, &priv::on_3dcanvas_mouse_dragging_finished, this);
    // 3DScene/Toolbar:
    view3D_canvas->Bind(EVT_GLTOOLBAR_ADD, &priv::on_action_add, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE, [q](SimpleEvent&) { q->remove_selected(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_DELETE_ALL, [this](SimpleEvent&) { reset(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_ARRANGE, [this](SimpleEvent&) { arrange(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_MORE, [q](SimpleEvent&) { q->increase_instances(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_FEWER, [q](SimpleEvent&) { q->decrease_instances(); });
    view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_OBJECTS, &priv::on_action_split_objects, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_SPLIT_VOLUMES, &priv::on_action_split_volumes, this);
    view3D_canvas->Bind(EVT_GLTOOLBAR_LAYERSEDITING, &priv::on_action_layersediting, this);
    view3D_canvas->Bind(EVT_GLCANVAS_INIT, [this](SimpleEvent&) { init_view_toolbar(); });

    // Preview events:
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_VIEWPORT_CHANGED, &priv::on_viewport_changed, this);
    preview->get_wxglcanvas()->Bind(EVT_GLCANVAS_QUESTION_MARK, [this](SimpleEvent&) { wxGetApp().keyboard_shortcuts(); });

    view3D_canvas->Bind(EVT_GLCANVAS_INIT, [this](SimpleEvent&) { init_view_toolbar(); });

    q->Bind(EVT_SLICING_COMPLETED, &priv::on_slicing_completed, this);
    q->Bind(EVT_PROCESS_COMPLETED, &priv::on_process_completed, this);
    q->Bind(EVT_GLVIEWTOOLBAR_3D, [q](SimpleEvent&) { q->select_view_3D("3D"); });
    q->Bind(EVT_GLVIEWTOOLBAR_PREVIEW, [q](SimpleEvent&) { q->select_view_3D("Preview"); });

    // Drop target:
    q->SetDropTarget(new PlaterDropTarget(q));   // if my understanding is right, wxWindow takes the owenership

    update_ui_from_settings();
    q->Layout();

    set_current_panel(view3D);
}

void Plater::priv::update(bool force_full_scene_refresh)
{
    wxWindowUpdateLocker freeze_guard(q);
    if (get_config("autocenter") == "1") {
        // auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
        // const auto bed_shape = Slic3r::Polygon::new_scale(bed_shape_opt->values);
        // const BoundingBox bed_shape_bb = bed_shape.bounding_box();
        const Vec2d& bed_center = bed_shape_bb().center();
        model.center_instances_around_point(bed_center);
    }

    unsigned int update_status = 0;
    if (this->printer_technology == ptSLA)
        // Update the SLAPrint from the current Model, so that the reload_scene()
        // pulls the correct data.
        update_status = this->update_background_process(false);
    this->view3D->reload_scene(false, force_full_scene_refresh);
	this->preview->reload_print();
    if (this->printer_technology == ptSLA)
        this->restart_background_process(update_status);
    else
        this->schedule_background_process();
}

void Plater::priv::select_view(const std::string& direction)
{
    if (current_panel == view3D)
        view3D->select_view(direction);
    else if (current_panel == preview)
        preview->select_view(direction);
}

void Plater::priv::select_view_3D(const std::string& name)
{
    if (name == "3D")
        set_current_panel(view3D);
    else if (name == "Preview")
        set_current_panel(preview);
}

void Plater::priv::select_next_view_3D()
{
    if (current_panel == view3D)
        set_current_panel(preview);
    else if (current_panel == preview)
        set_current_panel(view3D);
}

// Called after the Preferences dialog is closed and the program settings are saved.
// Update the UI based on the current preferences.
void Plater::priv::update_ui_from_settings()
{
    // TODO: (?)
    // my ($self) = @_;
    // if (defined($self->{btn_reslice}) && $self->{buttons_sizer}->IsShown($self->{btn_reslice}) != (! wxTheApp->{app_config}->get("background_processing"))) {
    //     $self->{buttons_sizer}->Show($self->{btn_reslice}, ! wxTheApp->{app_config}->get("background_processing"));
    //     $self->{buttons_sizer}->Layout;
    // }

#if ENABLE_RETINA_GL
    view3D->get_canvas3d()->update_ui_from_settings();
    preview->get_canvas3d()->update_ui_from_settings();
#endif
}

ProgressStatusBar* Plater::priv::statusbar()
{
    return main_frame->m_statusbar;
}

std::string Plater::priv::get_config(const std::string &key) const
{
    return wxGetApp().app_config->get(key);
}

BoundingBoxf Plater::priv::bed_shape_bb() const
{
    BoundingBox bb = scaled_bed_shape_bb();
    return BoundingBoxf(unscale(bb.min), unscale(bb.max));
}

BoundingBox Plater::priv::scaled_bed_shape_bb() const
{
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
    const auto bed_shape = Slic3r::Polygon::new_scale(bed_shape_opt->values);
    return bed_shape.bounding_box();
}

std::vector<size_t> Plater::priv::load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config)
{
    if (input_files.empty()) { return std::vector<size_t>(); }

    auto *nozzle_dmrs = config->opt<ConfigOptionFloats>("nozzle_diameter");

    bool one_by_one = input_files.size() == 1 || nozzle_dmrs->values.size() <= 1;
    if (! one_by_one) {
        for (const auto &path : input_files) {
            if (std::regex_match(path.string(), pattern_bundle)) {
                one_by_one = true;
                break;
            }
        }
    }

    const auto loading = _(L("Loading")) + dots;
    wxProgressDialog dlg(loading, loading);
    dlg.Pulse();

    auto *new_model = (!load_model || one_by_one) ? nullptr : new Slic3r::Model();
    std::vector<size_t> obj_idxs;

    for (size_t i = 0; i < input_files.size(); i++) {
        const auto &path = input_files[i];
        const auto filename = path.filename();
        const auto dlg_info = wxString::Format(_(L("Processing input file %s\n")), from_path(filename));
        dlg.Update(100 * i / input_files.size(), dlg_info);

        const bool type_3mf = std::regex_match(path.string(), pattern_3mf);
        const bool type_zip_amf = !type_3mf && std::regex_match(path.string(), pattern_zip_amf);
        const bool type_any_amf = !type_3mf && std::regex_match(path.string(), pattern_any_amf);
#if ENABLE_VOLUMES_CENTERING_FIXES
        const bool type_prusa = std::regex_match(path.string(), pattern_prusa);
#endif // ENABLE_VOLUMES_CENTERING_FIXES

        Slic3r::Model model;
        try {
            if (type_3mf || type_zip_amf) {
                DynamicPrintConfig config;
                {
                    DynamicPrintConfig config_loaded;
                    model = Slic3r::Model::read_from_archive(path.string(), &config_loaded, false);
                    if (load_config && !config_loaded.empty()) {
                        // Based on the printer technology field found in the loaded config, select the base for the config,
					    PrinterTechnology printer_technology = Preset::printer_technology(config_loaded);
					    config.apply(printer_technology == ptFFF ?
                            static_cast<const ConfigBase&>(FullPrintConfig::defaults()) : 
                            static_cast<const ConfigBase&>(SLAFullPrintConfig::defaults()));
                        // and place the loaded config over the base.
                        config += std::move(config_loaded);
                    }
                }

                if (load_config)
                {
                    if (!config.empty()) {
                        Preset::normalize(config);
                        wxGetApp().preset_bundle->load_config_model(filename.string(), std::move(config));
                        wxGetApp().load_current_presets();
                    }
                    wxGetApp().app_config->update_config_dir(path.parent_path().string());
                }
            }
            else {
                model = Slic3r::Model::read_from_file(path.string(), nullptr, false);
                for (auto obj : model.objects)
                    if (obj->name.empty())
                        obj->name = fs::path(obj->input_file).filename().string();
            }
        } catch (const std::exception &e) {
            GUI::show_error(q, e.what());
            continue;
        }

        if (load_model)
        {
            // The model should now be initialized

#if ENABLE_VOLUMES_CENTERING_FIXES
            if (!type_3mf && !type_any_amf && !type_prusa) {
#endif // ENABLE_VOLUMES_CENTERING_FIXES
                if (model.looks_like_multipart_object()) {
                    wxMessageDialog dlg(q, _(L(
                        "This file contains several objects positioned at multiple heights. "
                        "Instead of considering them as multiple objects, should I consider\n"
                        "this file as a single object having multiple parts?\n"
                        )), _(L("Multi-part object detected")), wxICON_WARNING | wxYES | wxNO);
                    if (dlg.ShowModal() == wxID_YES) {
                        model.convert_multipart_object(nozzle_dmrs->values.size());
                    }
                }
#if ENABLE_VOLUMES_CENTERING_FIXES
            }
#endif // ENABLE_VOLUMES_CENTERING_FIXES

#if !ENABLE_VOLUMES_CENTERING_FIXES
            if (type_3mf || type_any_amf) {
#endif // !ENABLE_VOLUMES_CENTERING_FIXES
                for (ModelObject* model_object : model.objects) {
                    model_object->center_around_origin();
                    model_object->ensure_on_bed();
                }
#if !ENABLE_VOLUMES_CENTERING_FIXES
            }
#endif // !ENABLE_VOLUMES_CENTERING_FIXES

            // check multi-part object adding for the SLA-printing
            if (printer_technology == ptSLA)
            {
                for (auto obj : model.objects)
                    if ( obj->volumes.size()>1 ) {
                        Slic3r::GUI::show_error(nullptr, 
                            wxString::Format(_(L("You can't to add the object(s) from %s because of one or some of them is(are) multi-part")), 
                                             from_path(filename)));
                        return std::vector<size_t>();
                    }
            }

            if (one_by_one) {
                auto loaded_idxs = load_model_objects(model.objects);
                obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
            } else {
                // This must be an .stl or .obj file, which may contain a maximum of one volume.
                for (const ModelObject* model_object : model.objects) {
                    new_model->add_object(*model_object);
                }
            }
        }
    }

    if (new_model != nullptr) {
        wxMessageDialog dlg(q, _(L(
                "Multiple objects were loaded for a multi-material printer.\n"
                "Instead of considering them as multiple objects, should I consider\n"
                "these files to represent a single object having multiple parts?\n"
            )), _(L("Multi-part object detected")), wxICON_WARNING | wxYES | wxNO);
        if (dlg.ShowModal() == wxID_YES) {
            new_model->convert_multipart_object(nozzle_dmrs->values.size());
        }

        auto loaded_idxs = load_model_objects(new_model->objects);
        obj_idxs.insert(obj_idxs.end(), loaded_idxs.begin(), loaded_idxs.end());
    }

    if (load_model)
    {
        wxGetApp().app_config->update_skein_dir(input_files[input_files.size() - 1].parent_path().string());
        // XXX: Plater.pm had @loaded_files, but didn't seem to fill them with the filenames...
        statusbar()->set_status_text(_(L("Loaded")));
    }

    // automatic selection of added objects
    if (!obj_idxs.empty() && (view3D != nullptr))
    {
        GLCanvas3D::Selection& selection = view3D->get_canvas3d()->get_selection();
        selection.clear();
        for (size_t idx : obj_idxs)
        {
            selection.add_object((unsigned int)idx, false);
        }
    }

    return obj_idxs;
}

// #define AUTOPLACEMENT_ON_LOAD

std::vector<size_t> Plater::priv::load_model_objects(const ModelObjectPtrs &model_objects)
{
    const BoundingBoxf bed_shape = bed_shape_bb();
    const Vec3d bed_size = Slic3r::to_3d(bed_shape.size().cast<double>(), 1.0) - 2.0 * Vec3d::Ones();

#ifndef AUTOPLACEMENT_ON_LOAD
    bool need_arrange = false;
#endif /* AUTOPLACEMENT_ON_LOAD */
    bool scaled_down = false;
    std::vector<size_t> obj_idxs;
    unsigned int obj_count = model.objects.size();

#ifdef AUTOPLACEMENT_ON_LOAD
    ModelInstancePtrs new_instances;
#endif /* AUTOPLACEMENT_ON_LOAD */
    for (ModelObject *model_object : model_objects) {
        auto *object = model.add_object(*model_object);
        std::string object_name = object->name.empty() ? fs::path(object->input_file).filename().string() : object->name;
        obj_idxs.push_back(obj_count++);

        if (model_object->instances.empty()) {
#ifdef AUTOPLACEMENT_ON_LOAD
            object->center_around_origin();
            new_instances.emplace_back(object->add_instance());
#else /* AUTOPLACEMENT_ON_LOAD */
            // if object has no defined position(s) we need to rearrange everything after loading               object->center_around_origin();
            need_arrange = true;                
             // add a default instance and center object around origin  
            object->center_around_origin();  // also aligns object to Z = 0 
            ModelInstance* instance = object->add_instance();   
            instance->set_offset(Slic3r::to_3d(bed_shape.center().cast<double>(), -object->origin_translation(2)));
#endif /* AUTOPLACEMENT_ON_LOAD */
        }

        const Vec3d size = object->bounding_box().size();
        const Vec3d ratio = size.cwiseQuotient(bed_size);
        const double max_ratio = std::max(ratio(0), ratio(1));
        if (max_ratio > 10000) {
            // the size of the object is too big -> this could lead to overflow when moving to clipper coordinates,
            // so scale down the mesh
			double inv = 1. / max_ratio;
            object->scale_mesh(Vec3d(inv, inv, inv));
            object->origin_translation = Vec3d::Zero();
            object->center_around_origin();
            scaled_down = true;
        } else if (max_ratio > 5) {
            const Vec3d inverse = 1.0 / max_ratio * Vec3d::Ones();
            for (ModelInstance *instance : object->instances) {
                instance->set_scaling_factor(inverse);
            }
            scaled_down = true;
        }

        object->ensure_on_bed();

        // print.auto_assign_extruders(object);
        // print.add_model_object(object);
    }

#ifdef AUTOPLACEMENT_ON_LOAD
    // FIXME distance should be a config value /////////////////////////////////
    auto min_obj_distance = static_cast<coord_t>(6/SCALING_FACTOR);
    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");
    assert(bed_shape_opt);
    auto& bedpoints = bed_shape_opt->values;
    Polyline bed; bed.points.reserve(bedpoints.size());
    for(auto& v : bedpoints) bed.append(Point::new_scale(v(0), v(1)));

    arr::find_new_position(model, new_instances, min_obj_distance, bed);
#endif /* AUTOPLACEMENT_ON_LOAD */

    if (scaled_down) {
        GUI::show_info(q,
            _(L("Your object appears to be too large, so it was automatically scaled down to fit your print bed.")),
            _(L("Object too large?")));
    }

    for (const size_t idx : obj_idxs) {
        wxGetApp().obj_list()->add_object_to_list(idx);
    }

    update();
    object_list_changed();

    this->schedule_background_process();

    return obj_idxs;
}

std::unique_ptr<CheckboxFileDialog> Plater::priv::get_export_file(GUI::FileType file_type)
{
    wxString wildcard;
    switch (file_type) {
        case FT_STL:
        case FT_AMF:
        case FT_3MF:
        case FT_GCODE:
            wildcard = file_wildcards(file_type);
        break;
        default:
            wildcard = file_wildcards(FT_MODEL);
        break;
    }

    // Update printbility state of each of the ModelInstances.
    this->update_print_volume_state();
    // Find the file name of the first printable object.
	fs::path output_file = this->model.propose_export_file_name_and_path();

    switch (file_type) {
        case FT_STL: output_file.replace_extension("stl"); break;
        case FT_AMF: output_file.replace_extension("zip.amf"); break;   // XXX: Problem on OS X with double extension?
        case FT_3MF: output_file.replace_extension("3mf"); break;
        default: break;
    }

    auto dlg = Slic3r::make_unique<CheckboxFileDialog>(q,
        ((file_type == FT_AMF) || (file_type == FT_3MF)) ? _(L("Export print config")) : "",
        true,
        _(L("Save file as:")),
        from_path(output_file.parent_path()),
        from_path(output_file.filename()),
        wildcard,
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    if (dlg->ShowModal() != wxID_OK) {
        return nullptr;
    }

    fs::path path(into_path(dlg->GetPath()));
    wxGetApp().app_config->update_last_output_dir(path.parent_path().string());

    return dlg;
}

const GLCanvas3D::Selection& Plater::priv::get_selection() const
{
    return view3D->get_canvas3d()->get_selection();
}

GLCanvas3D::Selection& Plater::priv::get_selection()
{
    return view3D->get_canvas3d()->get_selection();
}

int Plater::priv::get_selected_object_idx() const
{
    int idx = get_selection().get_object_idx();
    return ((0 <= idx) && (idx < 1000)) ? idx : -1;
}

int Plater::priv::get_selected_volume_idx() const
{
    auto& selection = get_selection();
    int idx = selection.get_object_idx();
    if ((0 > idx) || (idx > 1000)) 
        return-1;
    const GLVolume* v = selection.get_volume(*selection.get_volume_idxs().begin());
    if (model.objects[idx]->volumes.size() > 1)
        return v->volume_idx();
    return -1;
}

void Plater::priv::selection_changed()
{
    view3D->enable_toolbar_item("delete", can_delete_object());
    view3D->enable_toolbar_item("more", can_increase_instances());
    view3D->enable_toolbar_item("fewer", can_decrease_instances());
    view3D->enable_toolbar_item("splitobjects", can_split/*_to_objects*/());
    view3D->enable_toolbar_item("splitvolumes", can_split/*_to_volumes*/());

    // if the selection is not valid to allow for layer editing, we need to turn off the tool if it is running
    bool enable_layer_editing = layers_height_allowed();
    if (!enable_layer_editing && view3D->is_layers_editing_enabled()) {
        SimpleEvent evt(EVT_GLTOOLBAR_LAYERSEDITING);
        on_action_layersediting(evt);
    }

    view3D->enable_toolbar_item("layersediting", enable_layer_editing);

    // forces a frame render to update the view (to avoid a missed update if, for example, the context menu appears)
    view3D->render();
}

void Plater::priv::object_list_changed()
{
    // Enable/disable buttons depending on whether there are any objects on the platter.
    view3D->enable_toolbar_item("deleteall", can_delete_all());
    view3D->enable_toolbar_item("arrange", can_arrange());

    const bool export_in_progress = this->background_process.is_export_scheduled(); // || ! send_gcode_file.empty());
    // XXX: is this right?
    const bool model_fits = view3D->check_volumes_outside_state() == ModelInstance::PVS_Inside;

    sidebar->enable_buttons(!model.objects.empty() && !export_in_progress && model_fits);
}

void Plater::priv::select_all()
{
    view3D->select_all();
    this->sidebar->obj_list()->update_selections();
}

void Plater::priv::remove(size_t obj_idx)
{
    // Prevent toolpaths preview from rendering while we modify the Print object
    preview->set_enabled(false);

    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    model.delete_object(obj_idx);
    // Delete object from Sidebar list
    sidebar->obj_list()->delete_object_from_list(obj_idx);

    object_list_changed();
    update();
}


void Plater::priv::delete_object_from_model(size_t obj_idx)
{
    model.delete_object(obj_idx);
    object_list_changed();
    update();
}

void Plater::priv::reset()
{
    project_filename.Clear();

    // Prevent toolpaths preview from rendering while we modify the Print object
    preview->set_enabled(false);

    if (view3D->is_layers_editing_enabled())
        view3D->enable_layers_editing(false);

    // Stop and reset the Print content.
    this->background_process.reset();
    model.clear_objects();

    // Delete all objects from list on c++ side
    sidebar->obj_list()->delete_all_objects_from_list();
    object_list_changed();
    update();

    // The hiding of the slicing results, if shown, is not taken care by the background process, so we do it here
    this->sidebar->show_sliced_info_sizer(false);

    auto& config = wxGetApp().preset_bundle->project_config;
    config.option<ConfigOptionFloats>("colorprint_heights")->values.clear();
}

void Plater::priv::mirror(Axis axis)
{
    view3D->mirror_selection(axis);
}

void Plater::priv::arrange()
{
    // don't do anything if currently arranging. Then this is a re-entrance
    if(arranging.load()) return;

    // Guard the arrange process
    arranging.store(true);

    wxBusyCursor wait;

    // Disable the arrange button (to prevent reentrancies, we will call wxYied)
    view3D->enable_toolbar_item("arrange", can_arrange());

    this->background_process.stop();
    unsigned count = 0;
    for(auto obj : model.objects) count += obj->instances.size();

    auto prev_range = statusbar()->get_range();
    statusbar()->set_range(count);

    auto statusfn = [this, count] (unsigned st, const std::string& msg) {
        /* // In case we would run the arrange asynchronously
        wxCommandEvent event(EVT_PROGRESS_BAR);
        event.SetInt(st);
        event.SetString(msg);
        wxQueueEvent(this->q, event.Clone()); */
        statusbar()->set_progress(count - st);
        statusbar()->set_status_text(msg);

        // ok, this is dangerous, but we are protected by the atomic flag
        // 'arranging' and the arrange button is also disabled.
        // This call is needed for the cancel button to work.
        wxYieldIfNeeded();
    };

    statusbar()->set_cancel_callback([this, statusfn](){
        arranging.store(false);
        statusfn(0, L("Arranging canceled"));
    });

    static const std::string arrangestr = L("Arranging");

    // FIXME: I don't know how to obtain the minimum distance, it depends
    // on printer technology. I guess the following should work but it crashes.
    double dist = 6; //PrintConfig::min_object_distance(config);

    auto min_obj_distance = static_cast<coord_t>(dist/SCALING_FACTOR);

    const auto *bed_shape_opt = config->opt<ConfigOptionPoints>("bed_shape");

    assert(bed_shape_opt);
    auto& bedpoints = bed_shape_opt->values;
    Polyline bed; bed.points.reserve(bedpoints.size());
    for(auto& v : bedpoints) bed.append(Point::new_scale(v(0), v(1)));

    statusfn(0, arrangestr);

    try {
        arr::BedShapeHint hint;

        // TODO: from Sasha from GUI or
        hint.type = arr::BedShapeType::WHO_KNOWS;

        arr::arrange(model,
                     min_obj_distance,
                     bed,
                     hint,
                     false, // create many piles not just one pile
                     [statusfn](unsigned st) { statusfn(st, arrangestr); },
                     [this] () { return !arranging.load(); });
    } catch(std::exception& /*e*/) {
        GUI::show_error(this->q, L("Could not arrange model objects! "
                                   "Some geometries may be invalid."));
    }

    statusfn(0, L("Arranging done."));
    statusbar()->set_range(prev_range);
    statusbar()->set_cancel_callback(); // remove cancel button
    arranging.store(false);

    // We enable back the arrange button
    view3D->enable_toolbar_item("arrange", can_arrange());

    // Do a full refresh of scene tree, including regenerating all the GLVolumes.
    //FIXME The update function shall just reload the modified matrices.
    update(true);
}

// This method will find an optimal orientation for the currently selected item
// Very similar in nature to the arrange method above...
void Plater::priv::sla_optimize_rotation() {

    // TODO: we should decide whether to allow arrange when the search is
    // running we should probably disable explicit slicing and background
    // processing

    if(rotoptimizing.load()) return;
    rotoptimizing.store(true);

    int obj_idx = get_selected_object_idx();
    ModelObject * o = model.objects[obj_idx];

    background_process.stop();

    auto prev_range = statusbar()->get_range();
    statusbar()->set_range(100);

    auto stfn = [this] (unsigned st, const std::string& msg) {
        statusbar()->set_progress(st);
        statusbar()->set_status_text(msg);

        // could be problematic, but we need the cancel button.
        wxYieldIfNeeded();
    };

    statusbar()->set_cancel_callback([this, stfn](){
        rotoptimizing.store(false);
        stfn(0, L("Orientation search canceled"));
    });

    auto r = sla::find_best_rotation(
                *o, .005f,
                [stfn](unsigned s) { stfn(s, L("Searching for optimal orientation")); },
                [this](){ return !rotoptimizing.load(); }
    );

    if(rotoptimizing.load()) // wasn't canceled
    for(ModelInstance * oi : o->instances) oi->set_rotation({r[X], r[Y], r[Z]});

    // Correct the z offset of the object which was corrupted be the rotation
    o->ensure_on_bed();

    stfn(0, L("Orientation found."));
    statusbar()->set_range(prev_range);
    statusbar()->set_cancel_callback();
    rotoptimizing.store(false);

    update(true);
}

void Plater::priv::split_object()
{
    int obj_idx = get_selected_object_idx();
    if (obj_idx == -1)
        return;

    // we clone model object because split_object() adds the split volumes
    // into the same model object, thus causing duplicates when we call load_model_objects()
    Model new_model = model;
    ModelObject* current_model_object = new_model.objects[obj_idx];

    if (current_model_object->volumes.size() > 1)
    {
        Slic3r::GUI::warning_catcher(q, _(L("The selected object can't be split because it contains more than one volume/material.")));
        return;
    }

    wxBusyCursor wait;
    ModelObjectPtrs new_objects;
    current_model_object->split(&new_objects);
    if (new_objects.size() == 1)
        Slic3r::GUI::warning_catcher(q, _(L("The selected object couldn't be split because it contains only one part.")));
    else
    {
        unsigned int counter = 1;
        for (ModelObject* m : new_objects)
            m->name = current_model_object->name + "_" + std::to_string(counter++);

        remove(obj_idx);

        // load all model objects at once, otherwise the plate would be rearranged after each one
        // causing original positions not to be kept
        std::vector<size_t> idxs = load_model_objects(new_objects);

        // select newly added objects
        for (size_t idx : idxs)
        {
            get_selection().add_object((unsigned int)idx, false);
        }
    }
}

void Plater::priv::split_volume()
{
    wxGetApp().obj_list()->split();
}

void Plater::priv::schedule_background_process()
{
    // Trigger the timer event after 0.5s
    this->background_process_timer.Start(500, wxTIMER_ONE_SHOT);
    // Notify the Canvas3D that something has changed, so it may invalidate some of the layer editing stuff.
    this->view3D->get_canvas3d()->set_config(this->config);
}

void Plater::priv::update_print_volume_state()
{
    BoundingBox     bed_box_2D = get_extents(Polygon::new_scale(this->config->opt<ConfigOptionPoints>("bed_shape")->values));
    BoundingBoxf3   print_volume(unscale(bed_box_2D.min(0), bed_box_2D.min(1), 0.0), unscale(bed_box_2D.max(0), bed_box_2D.max(1), scale_(this->config->opt_float("max_print_height"))));
    // Allow the objects to protrude below the print bed, only the part of the object above the print bed will be sliced.
    print_volume.min(2) = -1e10;
    this->q->model().update_print_volume_state(print_volume);
}

// Update background processing thread from the current config and Model.
// Returns a bitmask of UpdateBackgroundProcessReturnState.
unsigned int Plater::priv::update_background_process(bool force_validation)
{
    // bitmap of enum UpdateBackgroundProcessReturnState
    unsigned int return_state = 0;

    // If the update_background_process() was not called by the timer, kill the timer, 
    // so the update_restart_background_process() will not be called again in vain.
    this->background_process_timer.Stop();
    // Update the "out of print bed" state of ModelInstances.
    this->update_print_volume_state();
    // Apply new config to the possibly running background task.
    bool               was_running = this->background_process.running();
    Print::ApplyStatus invalidated = this->background_process.apply(this->q->model(), wxGetApp().preset_bundle->full_config());

    // Just redraw the 3D canvas without reloading the scene to consume the update of the layer height profile.
    if (view3D->is_layers_editing_enabled())
        view3D->get_wxglcanvas()->Refresh();

    if (invalidated == Print::APPLY_STATUS_INVALIDATED) {
        // Some previously calculated data on the Print was invalidated.
        // Hide the slicing results, as the current slicing status is no more valid.
        this->sidebar->show_sliced_info_sizer(false);
        // Reset preview canvases. If the print has been invalidated, the preview canvases will be cleared.
        // Otherwise they will be just refreshed.
        switch (this->printer_technology) {
        case ptFFF:
            if (this->preview != nullptr)
                // If the preview is not visible, the following line just invalidates the preview,
                // but the G-code paths are calculated first once the preview is made visible.
                this->preview->reload_print();
            // We also need to reload 3D scene because of the wipe tower preview box
            if (this->config->opt_bool("wipe_tower"))
                return_state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;
            break;
        case ptSLA:
            return_state |= UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE;
            break;
        }
    }

    if ((invalidated != Print::APPLY_STATUS_UNCHANGED || force_validation) && ! this->background_process.empty()) {
        // The state of the Print changed, and it is non-zero. Let's validate it and give the user feedback on errors.
        std::string err = this->background_process.validate();
        if (err.empty()) {
			if (invalidated != Print::APPLY_STATUS_UNCHANGED && this->background_processing_enabled())
                return_state |= UPDATE_BACKGROUND_PROCESS_RESTART;
        } else {
            // The print is not valid.
            // The error returned from the Print needs to be translated into the local language.
            GUI::show_error(this->q, _(err));
            return_state |= UPDATE_BACKGROUND_PROCESS_INVALID;
        }
    }

    if (invalidated != Print::APPLY_STATUS_UNCHANGED && was_running && ! this->background_process.running() &&
        (return_state & UPDATE_BACKGROUND_PROCESS_RESTART) == 0) {
		// The background processing was killed and it will not be restarted.
		wxCommandEvent evt(EVT_PROCESS_COMPLETED);
		evt.SetInt(-1);
		// Post the "canceled" callback message, so that it will be processed after any possible pending status bar update messages.
		wxQueueEvent(GUI::wxGetApp().mainframe->m_plater, evt.Clone());
	}

    return return_state;
}

// Restart background processing thread based on a bitmask of UpdateBackgroundProcessReturnState.
bool Plater::priv::restart_background_process(unsigned int state)
{
	if ( ! this->background_process.empty() &&
		 (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) == 0 &&
		 ( ((state & UPDATE_BACKGROUND_PROCESS_FORCE_RESTART) != 0 && ! this->background_process.finished()) ||
           (state & UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT) != 0 ||
           (state & UPDATE_BACKGROUND_PROCESS_RESTART) != 0 ) ) {
        // The print is valid and it can be started.
        if (this->background_process.start()) {
            this->statusbar()->set_cancel_callback([this]() {
                this->statusbar()->set_status_text(L("Cancelling"));
                this->background_process.stop();
            });
            return true;
        }
    }
    return false;
}

void Plater::priv::export_gcode(fs::path output_path, PrintHostJob upload_job)
{
    wxCHECK_RET(!(output_path.empty() && upload_job.empty()), "export_gcode: output_path and upload_job empty");

    if (model.objects.empty())
        return;

    if (background_process.is_export_scheduled()) {
        GUI::show_error(q, _(L("Another export job is currently running.")));
        return;
    }

    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        view3D->reload_scene(false);

    if ((state & priv::UPDATE_BACKGROUND_PROCESS_INVALID) != 0)
        return;

    if (! output_path.empty()) {
        background_process.schedule_export(output_path.string());
    } else {
        background_process.schedule_upload(std::move(upload_job));
    }

    this->restart_background_process(priv::UPDATE_BACKGROUND_PROCESS_FORCE_EXPORT);
}

unsigned int Plater::priv::update_restart_background_process(bool force_update_scene, bool force_update_preview)
{
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->update_background_process(false);
    if (force_update_scene || (state & UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE) != 0)
        view3D->reload_scene(false);

    if (force_update_preview)
        this->preview->reload_print();
    this->restart_background_process(state);
	return state;
}

void Plater::priv::update_fff_scene()
{
    if (this->preview != nullptr)
        this->preview->reload_print();
    // In case this was MM print, wipe tower bounding box on 3D tab might need redrawing with exact depth:
	view3D->reload_scene(true);
}

void Plater::priv::update_sla_scene()
{
    // Update the SLAPrint from the current Model, so that the reload_scene()
    // pulls the correct data.
    delayed_scene_refresh = false;
    this->update_restart_background_process(true, true);
}

void Plater::priv::reload_from_disk()
{
    const auto &selection = get_selection();
    const auto obj_orig_idx = selection.get_object_idx();
    if (selection.is_wipe_tower() || obj_orig_idx == -1) { return; }

    auto *object_orig = model.objects[obj_orig_idx];
    std::vector<fs::path> input_paths(1, object_orig->input_file);

    const auto new_idxs = load_files(input_paths, true, false);

    for (const auto idx : new_idxs) {
        ModelObject *object = model.objects[idx];

        object->clear_instances();
        for (const ModelInstance *instance : object_orig->instances) {
            object->add_instance(*instance);
        }

        if (object->volumes.size() == object_orig->volumes.size()) {
            for (size_t i = 0; i < object->volumes.size(); i++) {
                object->volumes[i]->config.apply(object_orig->volumes[i]->config);
            }
        }

        // XXX: Restore more: layer_height_ranges, layer_height_profile (?)
    }

    remove(obj_orig_idx);
}

void Plater::priv::fix_through_netfabb(const int obj_idx, const int vol_idx/* = -1*/)
{
    if (obj_idx < 0)
        return;
    fix_model_by_win10_sdk_gui(*model.objects[obj_idx], vol_idx);
    this->object_list_changed();
    this->update();
    this->schedule_background_process();
}

void Plater::priv::set_current_panel(wxPanel* panel)
{
    if (std::find(panels.begin(), panels.end(), panel) == panels.end())
        return;

    if (current_panel == panel)
        return;

    current_panel = panel;
    // to reduce flickering when changing view, first set as visible the new current panel
    for (wxPanel* p : panels)
    {
        if (p == current_panel)
            p->Show();
    }
    // then set to invisible the other
    for (wxPanel* p : panels)
    {
        if (p != current_panel)
            p->Hide();
    }

    panel_sizer->Layout();

    if (current_panel == view3D)
    {
        if (view3D->is_reload_delayed())
        {
            // Delayed loading of the 3D scene.
            if (this->printer_technology == ptSLA)
            {
                // Update the SLAPrint from the current Model, so that the reload_scene()
                // pulls the correct data.
                this->update_restart_background_process(true, false);
            } else
                view3D->reload_scene(true);
        }
        // sets the canvas as dirty to force a render at the 1st idle event (wxWidgets IsShownOnScreen() is buggy and cannot be used reliably)
        view3D->set_as_dirty();
        view_toolbar.select_item("3D");
    }
    else if (current_panel == preview)
    {
        this->q->reslice();        
        preview->reload_print();
        preview->set_canvas_as_dirty();
        view_toolbar.select_item("Preview");
    }

    current_panel->SetFocusFromKbd();
}

void Plater::priv::on_select_preset(wxCommandEvent &evt)
{
    auto preset_type = static_cast<Preset::Type>(evt.GetInt());
    auto *combo = static_cast<PresetComboBox*>(evt.GetEventObject());

    auto idx = combo->get_extruder_idx();

    //! Because of The MSW and GTK version of wxBitmapComboBox derived from wxComboBox, 
    //! but the OSX version derived from wxOwnerDrawnCombo.
    //! So, to get selected string we do 
    //!     combo->GetString(combo->GetSelection()) 
    //! instead of 
    //!     combo->GetStringSelection().ToUTF8().data()); 

    std::string selected_string = combo->GetString(combo->GetSelection()).ToUTF8().data();

    if (preset_type == Preset::TYPE_FILAMENT) {
        wxGetApp().preset_bundle->set_filament_preset(idx, selected_string);
    }

    // TODO: ?
    if (preset_type == Preset::TYPE_FILAMENT && sidebar->is_multifilament()) {
        // Only update the platter UI for the 2nd and other filaments.
        wxGetApp().preset_bundle->update_platter_filament_ui(idx, combo);
    } 
    else {
        for (Tab* tab : wxGetApp().tabs_list) {
            if (tab->type() == preset_type) {
                tab->select_preset(selected_string);
                break;
            }
        }
    }

    // update plater with new config
    wxGetApp().plater()->on_config_change(wxGetApp().preset_bundle->full_config());
    if (preset_type == Preset::TYPE_PRINTER)
        wxGetApp().obj_list()->update_settings_items();
}

void Plater::priv::on_slicing_update(SlicingStatusEvent &evt)
{
    this->statusbar()->set_progress(evt.status.percent);
    this->statusbar()->set_status_text(_(L(evt.status.text)) + wxString::FromUTF8("…"));
    if (evt.status.flags & PrintBase::SlicingStatus::RELOAD_SCENE) {
        switch (this->printer_technology) {
        case ptFFF:
            this->update_fff_scene();
            break;
        case ptSLA:
            if (view3D->is_dragging())
                delayed_scene_refresh = true;
            else
                this->update_sla_scene();
            break;
        }
    }
}

void Plater::priv::on_slicing_completed(wxCommandEvent &)
{
    switch (this->printer_technology) {
    case ptFFF:
        this->update_fff_scene();
        break;
    case ptSLA:
        if (view3D->is_dragging())
            delayed_scene_refresh = true;
        else
            this->update_sla_scene();
        break;
    }
}

void Plater::priv::on_process_completed(wxCommandEvent &evt)
{
    // Stop the background task, wait until the thread goes into the "Idle" state.
    // At this point of time the thread should be either finished or canceled,
    // so the following call just confirms, that the produced data were consumed.
    this->background_process.stop();
    this->statusbar()->reset_cancel_callback();
    this->statusbar()->stop_busy();
  
	bool canceled = evt.GetInt() < 0;
    bool success  = evt.GetInt() > 0;
    // Reset the "export G-code path" name, so that the automatic background processing will be enabled again.
    this->background_process.reset_export();
    if (! success) {
        wxString message = evt.GetString();
        if (message.IsEmpty())
            message = _(L("Export failed"));
        this->statusbar()->set_status_text(message);
    }
	if (canceled)
		this->statusbar()->set_status_text(L("Cancelled"));

    this->sidebar->show_sliced_info_sizer(success);

    // This updates the "Slice now", "Export G-code", "Arrange" buttons status.
    // Namely, it refreshes the "Out of print bed" property of all the ModelObjects, and it enables
    // the "Slice now" and "Export G-code" buttons based on their "out of bed" status.
    this->object_list_changed();
    
    // refresh preview
    switch (this->printer_technology) {
    case ptFFF:
        this->update_fff_scene();
        break;
    case ptSLA:
        if (view3D->is_dragging())
            delayed_scene_refresh = true;
        else
            this->update_sla_scene();
        break;
    }
}

void Plater::priv::on_layer_editing_toggled(bool enable)
{
    view3D->enable_layers_editing(enable);
    if (enable && !view3D->is_layers_editing_enabled()) {
        // Initialization of the OpenGL shaders failed. Disable the tool.
        view3D->enable_toolbar_item("layersediting", false);
    }
    view3D->set_as_dirty();
}

void Plater::priv::on_action_add(SimpleEvent&)
{
    if (q != nullptr)
        q->add_model();
}

void Plater::priv::on_action_split_objects(SimpleEvent&)
{
    split_object();
}

void Plater::priv::on_action_split_volumes(SimpleEvent&)
{
    split_volume();
}

void Plater::priv::on_action_layersediting(SimpleEvent&)
{
    bool enable = !view3D->is_layers_editing_enabled();
    view3D->enable_layers_editing(enable);
    if (enable && !view3D->is_layers_editing_enabled())
        view3D->enable_toolbar_item("layersediting", false);
}

void Plater::priv::on_object_select(SimpleEvent& evt)
{
    wxGetApp().obj_list()->update_selections();
    selection_changed();
}

void Plater::priv::on_viewport_changed(SimpleEvent& evt)
{
    wxObject* o = evt.GetEventObject();
    if (o == preview->get_wxglcanvas())
        preview->set_viewport_into_scene(view3D->get_canvas3d());
    else if (o == view3D->get_wxglcanvas())
        preview->set_viewport_from_scene(view3D->get_canvas3d());
}

void Plater::priv::on_right_click(Vec2dEvent& evt)
{
    int obj_idx = get_selected_object_idx();
    if (obj_idx == -1)
        return;

    wxMenu* menu = printer_technology == ptSLA ? &sla_object_menu :
                   get_selection().is_single_full_instance() ? // show "Object menu" for each FullInstance instead of FullObject
                   &object_menu : &part_menu;

    sidebar->obj_list()->append_menu_item_settings(menu);

    if (q != nullptr)
        q->PopupMenu(menu, (int)evt.data.x(), (int)evt.data.y());
}

void Plater::priv::on_wipetower_moved(Vec3dEvent &evt)
{
    DynamicPrintConfig cfg;
    cfg.opt<ConfigOptionFloat>("wipe_tower_x", true)->value = evt.data(0);
    cfg.opt<ConfigOptionFloat>("wipe_tower_y", true)->value = evt.data(1);
    wxGetApp().get_tab(Preset::TYPE_PRINT)->load_config(cfg);
}

void Plater::priv::on_update_geometry(Vec3dsEvent<2>&)
{
    // TODO
}

// Update the scene from the background processing, 
// if the update message was received during mouse manipulation.
void Plater::priv::on_3dcanvas_mouse_dragging_finished(SimpleEvent&)
{
    if (this->delayed_scene_refresh) {
        this->delayed_scene_refresh = false;
        this->update_sla_scene();
    }
}

bool Plater::priv::init_object_menu()
{
    init_common_menu(&object_menu);
    complit_init_object_menu();

    init_common_menu(&sla_object_menu);
    complit_init_sla_object_menu();

    init_common_menu(&part_menu, true);
    complit_init_part_menu();

    return true;
}

bool Plater::priv::init_common_menu(wxMenu* menu, const bool is_part/* = false*/)
{
    wxMenuItem* item_delete = append_menu_item(menu, wxID_ANY, _(L("Delete")) + "\tDel", _(L("Remove the selected object")),
        [this](wxCommandEvent&) { q->remove_selected(); }, "brick_delete.png");
    if (!is_part){
        wxMenuItem* item_increase = append_menu_item(menu, wxID_ANY, _(L("Increase copies")) + "\t+", _(L("Place one more copy of the selected object")),
            [this](wxCommandEvent&) { q->increase_instances(); }, "add.png");
        wxMenuItem* item_decrease = append_menu_item(menu, wxID_ANY, _(L("Decrease copies")) + "\t-", _(L("Remove one copy of the selected object")),
            [this](wxCommandEvent&) { q->decrease_instances(); }, "delete.png");
        wxMenuItem* item_set_number_of_copies = append_menu_item(menu, wxID_ANY, _(L("Set number of copies")) + dots, _(L("Change the number of copies of the selected object")),
            [this](wxCommandEvent&) { q->set_number_of_copies(); }, "textfield.png");

        menu->AppendSeparator();
        wxMenuItem* item_instance_to_object = sidebar->obj_list()->append_menu_item_instance_to_object(menu);

        if (q != nullptr)
        {
            q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_increase_instances()); }, item_increase->GetId());
            q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_decrease_instances()); }, item_decrease->GetId());
            q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_increase_instances()); }, item_set_number_of_copies->GetId());
            q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_set_instance_to_object()); }, item_instance_to_object->GetId());
        }
        menu->AppendSeparator();

        append_menu_item(menu, wxID_ANY, _(L("Reload from Disk")), _(L("Reload the selected file from Disk")),
            [this](wxCommandEvent&) { reload_from_disk(); });

        append_menu_item(menu, wxID_ANY, _(L("Export object as STL")) + dots, _(L("Export this single object as STL file")),
            [this](wxCommandEvent&) { q->export_stl(true); });
    }
    menu->AppendSeparator();

    sidebar->obj_list()->append_menu_item_fix_through_netfabb(menu);

    wxMenu* mirror_menu = new wxMenu();
    if (mirror_menu == nullptr)
        return false;

    append_menu_item(mirror_menu, wxID_ANY, _(L("Along X axis")), _(L("Mirror the selected object along the X axis")),
        [this](wxCommandEvent&) { mirror(X); }, "bullet_red.png", menu);
    append_menu_item(mirror_menu, wxID_ANY, _(L("Along Y axis")), _(L("Mirror the selected object along the Y axis")),
        [this](wxCommandEvent&) { mirror(Y); }, "bullet_green.png", menu);
    append_menu_item(mirror_menu, wxID_ANY, _(L("Along Z axis")), _(L("Mirror the selected object along the Z axis")),
        [this](wxCommandEvent&) { mirror(Z); }, "bullet_blue.png", menu);

    wxMenuItem* item_mirror = append_submenu(menu, mirror_menu, wxID_ANY, _(L("Mirror")), _(L("Mirror the selected object")));

    // ui updates needs to be binded to the parent panel
    if (q != nullptr)
    {
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_mirror()); }, item_mirror->GetId());
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_delete_object()); }, item_delete->GetId());
    }

    return true;
}

bool Plater::priv::complit_init_object_menu()
{
    wxMenu* split_menu = new wxMenu();
    if (split_menu == nullptr)
        return false;

    wxMenuItem* item_split_objects = append_menu_item(split_menu, wxID_ANY, _(L("To objects")), _(L("Split the selected object into individual objects")),
        [this](wxCommandEvent&) { split_object(); }, "shape_ungroup_o.png", &object_menu);
    wxMenuItem* item_split_volumes = append_menu_item(split_menu, wxID_ANY, _(L("To parts")), _(L("Split the selected object into individual sub-parts")),
        [this](wxCommandEvent&) { split_volume(); }, "shape_ungroup_p.png", &object_menu);

    wxMenuItem* item_split = append_submenu(&object_menu, split_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object")), "shape_ungroup.png");
    object_menu.AppendSeparator();

    // "Add (volumes)" popupmenu will be added later in append_menu_items_add_volume()

    // ui updates needs to be binded to the parent panel
    if (q != nullptr)
    {
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_split()); }, item_split->GetId());
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_split()); }, item_split_objects->GetId());
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_split()); }, item_split_volumes->GetId());
    }
    return true;
}

bool Plater::priv::complit_init_sla_object_menu()
{
    wxMenuItem* item_split = append_menu_item(&sla_object_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object into individual objects")),
        [this](wxCommandEvent&) { split_object(); }, "shape_ungroup_o.png");

    sla_object_menu.AppendSeparator();

    // Add the automatic rotation sub-menu
    append_menu_item(&sla_object_menu, wxID_ANY, _(L("Optimize orientation")), _(L("Optimize the rotation of the object for better print results.")),
        [this](wxCommandEvent&) { sla_optimize_rotation(); });

    // ui updates needs to be binded to the parent panel
    if (q != nullptr)
    {
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_split()); }, item_split->GetId());
    }

    return true;
}

bool Plater::priv::complit_init_part_menu()
{
    wxMenuItem* item_split = append_menu_item(&part_menu, wxID_ANY, _(L("Split")), _(L("Split the selected object into individual sub-parts")),
        [this](wxCommandEvent&) { split_volume(); }, "shape_ungroup_p.png");

    part_menu.AppendSeparator();

    auto obj_list = sidebar->obj_list();
    obj_list->append_menu_item_change_type(&part_menu);

    // ui updates needs to be binded to the parent panel
    if (q != nullptr)
    {
        q->Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& evt) { evt.Enable(can_split()); },  item_split->GetId());
    }

    return true;
}

void Plater::priv::init_view_toolbar()
{
    ItemsIconsTexture::Metadata icons_data;
    icons_data.filename = "view_toolbar.png";
    icons_data.icon_size = 64;
    icons_data.icon_border_size = 0;
    icons_data.icon_gap_size = 0;

    BackgroundTexture::Metadata background_data;
    background_data.filename = "toolbar_background.png";
    background_data.left = 16;
    background_data.top = 16;
    background_data.right = 16;
    background_data.bottom = 16;

    if (!view_toolbar.init(icons_data, background_data))
        return;

    view_toolbar.set_layout_orientation(GLToolbar::Layout::Bottom);
    view_toolbar.set_border(5.0f);
    view_toolbar.set_gap_size(1.0f);

    GLToolbarItem::Data item;

    item.name = "3D";
    item.tooltip = GUI::L_str("3D editor view") + " [" + GUI::shortkey_ctrl_prefix() + "5]";
    item.sprite_id = 0;
    item.action_event = EVT_GLVIEWTOOLBAR_3D;
    item.is_toggable = false;
    if (!view_toolbar.add_item(item))
        return;

    item.name = "Preview";
    item.tooltip = GUI::L_str("Preview") + " [" + GUI::shortkey_ctrl_prefix() + "6]";
    item.sprite_id = 1;
    item.action_event = EVT_GLVIEWTOOLBAR_PREVIEW;
    item.is_toggable = false;
    if (!view_toolbar.add_item(item))
        return;

    view_toolbar.enable_item("3D");
    view_toolbar.enable_item("Preview");

    view_toolbar.select_item("3D");
    view_toolbar.set_enabled(true);

    view3D->set_view_toolbar(&view_toolbar);
    preview->set_view_toolbar(&view_toolbar);
}

bool Plater::priv::can_delete_object() const
{
    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size());
}

bool Plater::priv::can_increase_instances() const
{
    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size());
}

bool Plater::priv::can_set_instance_to_object() const
{
    const int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && (model.objects[obj_idx]->instances.size() > 1);
}

bool Plater::priv::can_decrease_instances() const
{
    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && (model.objects[obj_idx]->instances.size() > 1);
}

bool Plater::priv::can_split_to_objects() const
{
    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && !model.objects[obj_idx]->is_multiparts();
}

bool Plater::priv::can_split_to_volumes() const
{
    if (printer_technology == ptSLA)
        return false;
//     int obj_idx = get_selected_object_idx();
//     return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && !model.objects[obj_idx]->is_multiparts();
    return sidebar->obj_list()->is_splittable();
}

bool Plater::priv::can_split() const
{
    return sidebar->obj_list()->is_splittable();
}

bool Plater::priv::layers_height_allowed() const
{
    int obj_idx = get_selected_object_idx();
    return (0 <= obj_idx) && (obj_idx < (int)model.objects.size()) && config->opt_bool("variable_layer_height") && view3D->is_layers_editing_allowed();
}

bool Plater::priv::can_delete_all() const
{
    return !model.objects.empty();
}

bool Plater::priv::can_arrange() const
{
    return !model.objects.empty() && !arranging.load();
}

bool Plater::priv::can_mirror() const
{
    return get_selection().is_from_single_instance();
}

void Plater::priv::update_object_menu()
{
    sidebar->obj_list()->append_menu_items_add_volume(&object_menu);
#if ENABLE_MODE_AWARE_TOOLBAR_ITEMS
    if (view3D != nullptr)
        view3D->update_toolbar_items_visibility();
#endif // ENABLE_MODE_AWARE_TOOLBAR_ITEMS
}

// Plater / Public

Plater::Plater(wxWindow *parent, MainFrame *main_frame)
    : wxPanel(parent), p(new priv(this, main_frame))
{
    // Initialization performed in the private c-tor
}

Plater::~Plater()
{
}

Sidebar&        Plater::sidebar()           { return *p->sidebar; }
Model&          Plater::model()             { return p->model; }
const Print&    Plater::fff_print() const   { return p->fff_print; }
Print&          Plater::fff_print()         { return p->fff_print; }
const SLAPrint& Plater::sla_print() const   { return p->sla_print; }
SLAPrint&       Plater::sla_print()         { return p->sla_print; }

void Plater::load_project()
{
    wxString input_file;
    wxGetApp().load_project(this, input_file);

    if (input_file.empty())
        return;

    p->reset();
    p->project_filename = input_file;

    std::vector<fs::path> input_paths;
    input_paths.push_back(into_path(input_file));
    load_files(input_paths);
}

void Plater::add_model()
{
    wxArrayString input_files;
    wxGetApp().import_model(this, input_files);
    if (input_files.empty())
        return;

    std::vector<fs::path> input_paths;
    for (const auto &file : input_files) {
        input_paths.push_back(into_path(file));
    }
    load_files(input_paths, true, false);
}

void Plater::extract_config_from_project()
{
    wxString input_file;
    wxGetApp().load_project(this, input_file);

    if (input_file.empty())
        return;

    std::vector<fs::path> input_paths;
    input_paths.push_back(into_path(input_file));
    load_files(input_paths, false, true);
}

void Plater::load_files(const std::vector<fs::path>& input_files, bool load_model, bool load_config) { p->load_files(input_files, load_model, load_config); }

// To be called when providing a list of files to the GUI slic3r on command line.
void Plater::load_files(const std::vector<std::string>& input_files, bool load_model, bool load_config)
{ 
    std::vector<fs::path> paths;
    paths.reserve(input_files.size());
    for (const std::string &path : input_files)
        paths.emplace_back(path);
    p->load_files(paths, load_model, load_config);
}

void Plater::update() { p->update(); }

void Plater::update_ui_from_settings() { p->update_ui_from_settings(); }

void Plater::select_view(const std::string& direction) { p->select_view(direction); }

void Plater::select_view_3D(const std::string& name) { p->select_view_3D(name); }

void Plater::select_all() { p->select_all(); }

void Plater::remove(size_t obj_idx) { p->remove(obj_idx); }
void Plater::reset() { p->reset(); }

void Plater::delete_object_from_model(size_t obj_idx) { p->delete_object_from_model(obj_idx); }

void Plater::remove_selected()
{
    this->p->view3D->delete_selected();
}

void Plater::increase_instances(size_t num)
{
    int obj_idx = p->get_selected_object_idx();
    if (obj_idx == -1)
        return;

    ModelObject* model_object = p->model.objects[obj_idx];
    ModelInstance* model_instance = model_object->instances.back();

    bool was_one_instance = model_object->instances.size()==1;
        
    float offset = 10.0;
    for (size_t i = 0; i < num; i++, offset += 10.0) {
        Vec3d offset_vec = model_instance->get_offset() + Vec3d(offset, offset, 0.0);
        model_object->add_instance(offset_vec, model_instance->get_scaling_factor(), model_instance->get_rotation(), model_instance->get_mirror());
//        p->print.get_object(obj_idx)->add_copy(Slic3r::to_2d(offset_vec));
    }

    sidebar().obj_list()->increase_object_instances(obj_idx, was_one_instance ? num + 1 : num);

    if (p->get_config("autocenter") == "1") {
        p->arrange();
    } else {
        p->update();
    }

    p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    p->selection_changed();

    this->p->schedule_background_process();
}

void Plater::decrease_instances(size_t num)
{
    int obj_idx = p->get_selected_object_idx();
    if (obj_idx == -1)
        return;

    ModelObject* model_object = p->model.objects[obj_idx];
    if (model_object->instances.size() > num) {
        for (size_t i = 0; i < num; ++ i)
            model_object->delete_last_instance();
        sidebar().obj_list()->decrease_object_instances(obj_idx, num);
    }
    else {
        remove(obj_idx);
    }

    p->update();

    if (!model_object->instances.empty())
        p->get_selection().add_instance(obj_idx, (int)model_object->instances.size() - 1);

    p->selection_changed();
    this->p->schedule_background_process();
}

void Plater::set_number_of_copies(/*size_t num*/)
{
    int obj_idx = p->get_selected_object_idx();
    if (obj_idx == -1)
        return;

    ModelObject* model_object = p->model.objects[obj_idx];

    const auto num = wxGetNumberFromUser( " ", _("Enter the number of copies:"), 
                                    _("Copies of the selected object"), model_object->instances.size(), 0, 1000, this );
    if (num < 0)
        return;

    int diff = (int)num - (int)model_object->instances.size();
    if (diff > 0)
        increase_instances(diff);
    else if (diff < 0)
        decrease_instances(-diff);
}

bool Plater::is_selection_empty() const
{
    return p->get_selection().is_empty();
}

void Plater::cut(size_t obj_idx, size_t instance_idx, coordf_t z, bool keep_upper, bool keep_lower, bool rotate_lower)
{
    wxCHECK_RET(obj_idx < p->model.objects.size(), "obj_idx out of bounds");
    auto *object = p->model.objects[obj_idx];

    wxCHECK_RET(instance_idx < object->instances.size(), "instance_idx out of bounds");

    if (!keep_upper && !keep_lower) {
        return;
    }

    wxBusyCursor wait;
    const auto new_objects = object->cut(instance_idx, z, keep_upper, keep_lower, rotate_lower);

    remove(obj_idx);
    p->load_model_objects(new_objects);
}

void Plater::export_gcode()
{
    if (p->model.objects.empty())
        return;

    // If possible, remove accents from accented latin characters.
    // This function is useful for generating file names to be processed by legacy firmwares.
    fs::path default_output_file;
    try {
		// Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
		// Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
		unsigned int state = this->p->update_restart_background_process(false, false);
		if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
			return;
		default_output_file = this->p->background_process.current_print()->output_filepath("");
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    auto start_dir = wxGetApp().app_config->get_last_output_dir(default_output_file.parent_path().string());

    wxFileDialog dlg(this, (printer_technology() == ptFFF) ? _(L("Save G-code file as:")) : _(L("Save Zip file as:")),
        start_dir,
        from_path(default_output_file.filename()),
        GUI::file_wildcards((printer_technology() == ptFFF) ? FT_GCODE : FT_PNGZIP, default_output_file.extension().string()),
        wxFD_SAVE | wxFD_OVERWRITE_PROMPT
    );

    fs::path output_path;
    if (dlg.ShowModal() == wxID_OK) {
        fs::path path = into_path(dlg.GetPath());
        wxGetApp().app_config->update_last_output_dir(path.parent_path().string());
        output_path = std::move(path);
    }
    if (! output_path.empty())
        p->export_gcode(std::move(output_path), PrintHostJob());
}

void Plater::export_stl(bool selection_only)
{
    if (p->model.objects.empty()) { return; }

    auto dialog = p->get_export_file(FT_STL);
    if (! dialog) { return; }

    // Store a binary STL
    const wxString path = dialog->GetPath();
    const std::string path_u8 = into_u8(path);

    TriangleMesh mesh;
    if (selection_only) {
        const auto &selection = p->get_selection();
        if (selection.is_wipe_tower()) { return; }

        const auto obj_idx = selection.get_object_idx();
        if (obj_idx == -1) { return; }
        mesh = p->model.objects[obj_idx]->mesh();
    } else {
        mesh = p->model.mesh();
    }

    Slic3r::store_stl(path_u8.c_str(), &mesh, true);
    p->statusbar()->set_status_text(wxString::Format(_(L("STL file exported to %s")), path));
}

void Plater::export_amf()
{
    if (p->model.objects.empty()) { return; }

    auto dialog = p->get_export_file(FT_AMF);
    if (! dialog) { return; }

    const wxString path = dialog->GetPath();
    const std::string path_u8 = into_u8(path);

	DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    wxBusyCursor wait;
	if (Slic3r::store_amf(path_u8.c_str(), &p->model, dialog->get_checkbox_value() ? &cfg : nullptr)) {
        // Success
        p->statusbar()->set_status_text(wxString::Format(_(L("AMF file exported to %s")), path));
    } else {
        // Failure
        p->statusbar()->set_status_text(wxString::Format(_(L("Error exporting AMF file %s")), path));
    }
}

void Plater::export_3mf(const boost::filesystem::path& output_path)
{
    if (p->model.objects.empty()) { return; }

    wxString path;
    bool export_config = true;
    if (output_path.empty())
    {
        auto dialog = p->get_export_file(FT_3MF);
        if (!dialog) { return; }
        path = dialog->GetPath();
        export_config = dialog->get_checkbox_value();
    }
    else
        path = from_path(output_path);

    if (!path.Lower().EndsWith(".3mf"))
        return;

	DynamicPrintConfig cfg = wxGetApp().preset_bundle->full_config_secure();
    const std::string path_u8 = into_u8(path);
    wxBusyCursor wait;
    if (Slic3r::store_3mf(path_u8.c_str(), &p->model, export_config ? &cfg : nullptr)) {
        // Success
        p->statusbar()->set_status_text(wxString::Format(_(L("3MF file exported to %s")), path));
    } else {
        // Failure
        p->statusbar()->set_status_text(wxString::Format(_(L("Error exporting 3MF file %s")), path));
    }
}

void Plater::reslice()
{
    //FIXME Don't reslice if export of G-code or sending to OctoPrint is running.
    // bitmask of UpdateBackgroundProcessReturnState
    unsigned int state = this->p->update_background_process(true);
    if (state & priv::UPDATE_BACKGROUND_PROCESS_REFRESH_SCENE)
        this->p->view3D->reload_scene(false);
    // Only restarts if the state is valid.
    this->p->restart_background_process(state | priv::UPDATE_BACKGROUND_PROCESS_FORCE_RESTART);
}

void Plater::send_gcode()
{
    if (p->model.objects.empty()) { return; }

    PrintHostJob upload_job(p->config);
    if (upload_job.empty()) { return; }

    // Obtain default output path
    fs::path default_output_file;
    try {
		// Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
		// Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
		unsigned int state = this->p->update_restart_background_process(false, false);
		if (state & priv::UPDATE_BACKGROUND_PROCESS_INVALID)
			return;
		default_output_file = this->p->background_process.current_print()->output_filepath("");
    } catch (const std::exception &ex) {
        show_error(this, ex.what());
        return;
    }
    default_output_file = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));

    PrintHostSendDialog dlg(default_output_file);
    if (dlg.ShowModal() == wxID_OK) {
        upload_job.upload_data.upload_path = dlg.filename();
        upload_job.upload_data.start_print = dlg.start_print();

        p->export_gcode(fs::path(), std::move(upload_job));
    }
}

void Plater::on_extruders_change(int num_extruders)
{
    auto& choices = sidebar().combos_filament();

    wxWindowUpdateLocker noUpdates_scrolled_panel(&sidebar()/*.scrolled_panel()*/);
//     sidebar().scrolled_panel()->Freeze();

    int i = choices.size();
    while ( i < num_extruders )
    {
        PresetComboBox* choice/*{ nullptr }*/;
        sidebar().init_filament_combo(&choice, i);
        choices.push_back(choice);

        // initialize selection
        wxGetApp().preset_bundle->update_platter_filament_ui(i, choice);
        ++i;
    }

    // remove unused choices if any
    sidebar().remove_unused_filament_combos(num_extruders);

    sidebar().Layout();
    sidebar().scrolled_panel()->Refresh();
}

void Plater::on_config_change(const DynamicPrintConfig &config)
{
    bool update_scheduled = false;
#if ENABLE_REWORKED_BED_SHAPE_CHANGE
    bool bed_shape_changed = false;
#endif // ENABLE_REWORKED_BED_SHAPE_CHANGE
    for (auto opt_key : p->config->diff(config)) {
        p->config->set_key_value(opt_key, config.option(opt_key)->clone());
        if (opt_key == "printer_technology")
            this->set_printer_technology(config.opt_enum<PrinterTechnology>(opt_key));
        else if (opt_key == "bed_shape") {
#if ENABLE_REWORKED_BED_SHAPE_CHANGE
            bed_shape_changed = true;
#else
            if (p->view3D) p->view3D->set_bed_shape(p->config->option<ConfigOptionPoints>(opt_key)->values);
            if (p->preview) p->preview->set_bed_shape(p->config->option<ConfigOptionPoints>(opt_key)->values);
#endif // ENABLE_REWORKED_BED_SHAPE_CHANGE
            update_scheduled = true;
        } 
        else if (boost::starts_with(opt_key, "wipe_tower") ||
            // opt_key == "filament_minimal_purge_on_wipe_tower" // ? #ys_FIXME
            opt_key == "single_extruder_multi_material") {
            update_scheduled = true;
        } 
        else if(opt_key == "variable_layer_height") {
            if (p->config->opt_bool("variable_layer_height") != true) {
                p->view3D->enable_toolbar_item("layersediting", false);
                p->view3D->enable_layers_editing(false);
                p->view3D->set_as_dirty();
            }
            else if (p->view3D->is_layers_editing_allowed()) {
                p->view3D->enable_toolbar_item("layersediting", true);
            }
        } 
        else if(opt_key == "extruder_colour") {
            update_scheduled = true;
            p->preview->set_number_extruders(p->config->option<ConfigOptionStrings>(opt_key)->values.size());
        } else if(opt_key == "max_print_height") {
            update_scheduled = true;
        }
        else if (opt_key == "printer_model") {
            // update to force bed selection(for texturing)
#if ENABLE_REWORKED_BED_SHAPE_CHANGE
            bed_shape_changed = true;
#else
            if (p->view3D) p->view3D->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values);
            if (p->preview) p->preview->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values);
#endif // ENABLE_REWORKED_BED_SHAPE_CHANGE
            update_scheduled = true;
        }
        else if (opt_key == "host_type" && this->p->printer_technology == ptSLA) {
            p->config->option<ConfigOptionEnum<PrintHostType>>(opt_key)->value = htSL1;
        }
    }

    {
        const auto prin_host_opt = p->config->option<ConfigOptionString>("print_host");
        p->sidebar->show_send(prin_host_opt != nullptr && !prin_host_opt->value.empty());
    }

#if ENABLE_REWORKED_BED_SHAPE_CHANGE
    if (bed_shape_changed)
    {
        if (p->view3D) p->view3D->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values);
        if (p->preview) p->preview->set_bed_shape(p->config->option<ConfigOptionPoints>("bed_shape")->values);
    }
#endif // ENABLE_REWORKED_BED_SHAPE_CHANGE

    if (update_scheduled) 
        update();

    if (p->main_frame->is_loaded())
        this->p->schedule_background_process();
}

const wxString& Plater::get_project_filename() const
{
    return p->project_filename;
}

bool Plater::is_export_gcode_scheduled() const
{
    return p->background_process.is_export_scheduled();
}

int Plater::get_selected_object_idx()
{
    return p->get_selected_object_idx();
}

bool Plater::is_single_full_object_selection() const
{
    return p->get_selection().is_single_full_object();
}

GLCanvas3D* Plater::canvas3D()
{
    return p->view3D->get_canvas3d();
}

PrinterTechnology Plater::printer_technology() const
{
    return p->printer_technology;
}

void Plater::set_printer_technology(PrinterTechnology printer_technology)
{
    p->printer_technology = printer_technology;
    if (p->background_process.select_technology(printer_technology)) {
        // Update the active presets.
    }
    //FIXME for SLA synchronize 
    //p->background_process.apply(Model)!
}

void Plater::changed_object(int obj_idx)
{
    if (obj_idx < 0)
        return;
    auto list = wxGetApp().obj_list();
    wxASSERT(list != nullptr);
    if (list == nullptr)
        return;

    if (list->is_parts_changed()) {
        // recenter and re - align to Z = 0
        auto model_object = p->model.objects[obj_idx];
        model_object->ensure_on_bed();
        if (this->p->printer_technology == ptSLA) {
            // Update the SLAPrint from the current Model, so that the reload_scene()
            // pulls the correct data, update the 3D scene.
            this->p->update_restart_background_process(true, false);
        } else
            p->view3D->reload_scene(false);
    }

    // update print
    this->p->schedule_background_process();
}

void Plater::fix_through_netfabb(const int obj_idx, const int vol_idx/* = -1*/) { p->fix_through_netfabb(obj_idx, vol_idx); }

void Plater::update_object_menu() { p->update_object_menu(); }

}}    // namespace Slic3r::GUI
