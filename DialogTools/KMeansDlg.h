/**
 * GeoDa TM, Copyright (C) 2011-2015 by Luc Anselin - all rights reserved
 *
 * This file is part of GeoDa.
 *
 * GeoDa is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GeoDa is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __GEODA_CENTER_KMEAN_DLG_H___
#define __GEODA_CENTER_KMEAN_DLG_H___

#include <vector>
#include <map>

#include "../VarTools.h"

class Project;
class TableInterface;

class KMeansDlg : public wxDialog
{
public:
    KMeansDlg(wxFrame *parent, Project* project);
    virtual ~KMeansDlg();
    
    void CreateControls();
    bool Init();
    
    void OnOK( wxCommandEvent& event );
    void OnSave( wxCommandEvent& event );
    void OnClose( wxCommandEvent& event );
    
    void InitVariableCombobox(wxListBox* var_box);
    
    std::vector<GdaVarTools::VarInfo> var_info;
    std::vector<int> col_ids;
    
private:
    wxFrame *parent;
    Project* project;
    TableInterface* table_int;
    std::vector<wxString> tm_strs;
    
    wxListBox* combo_var;
    wxComboBox* combo_n;
    wxComboBox* combo_cov;
    wxTextCtrl* m_textbox;
    wxCheckBox* m_use_centroids;
    wxTextCtrl* m_iterations;
    wxComboBox* m_method;
    wxComboBox* m_distance;
    
    std::map<wxString, wxString> name_to_nm;
    std::map<wxString, int> name_to_tm_id;
    
    unsigned int row_lim;
    unsigned int col_lim;
    std::vector<float> scores;
    double thresh95;
};

#endif
