/*
 *  Copyright © 2017-2020 Wellington Wallace
 *
 *  This file is part of PulseEffects.
 *
 *  PulseEffects is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  PulseEffects is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with PulseEffects.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "effects_base_ui.hpp"

NodeInfoHolder::NodeInfoHolder(NodeInfo info)
    : Glib::ObjectBase(typeid(NodeInfoHolder)), Glib::Object(), info(std::move(info)) {}

auto NodeInfoHolder::create(const NodeInfo& info) -> Glib::RefPtr<NodeInfoHolder> {
  return Glib::make_refptr_for_instance<NodeInfoHolder>(new NodeInfoHolder(info));
}

EffectsBaseUi::EffectsBaseUi(const Glib::RefPtr<Gtk::Builder>& builder,
                             EffectsBase* effects_base,
                             const std::string& schema)
    : effects_base(effects_base),
      schema(schema),
      settings(Gio::Settings::create(schema)),
      pm(effects_base->pm),
      players_model(Gio::ListStore<NodeInfoHolder>::create()),
      all_players_model(Gio::ListStore<NodeInfoHolder>::create()),
      blocklist(Gtk::StringList::create({"initial_value"})),
      plugins(Gtk::StringList::create({"initial_value"})),
      selected_plugins(Gtk::StringList::create({"initial_value"})) {
  // loading builder widgets

  global_output_level_left = builder->get_widget<Gtk::Label>("global_output_level_left");
  global_output_level_right = builder->get_widget<Gtk::Label>("global_output_level_right");
  device_state = builder->get_widget<Gtk::Label>("device_state");
  saturation_icon = builder->get_widget<Gtk::Image>("saturation_icon");
  listview_players = builder->get_widget<Gtk::ListView>("listview_players");
  menubutton_blocklist = builder->get_widget<Gtk::MenuButton>("menubutton_blocklist");
  stack_top = builder->get_widget<Gtk::Stack>("stack_top");
  toggle_players = builder->get_widget<Gtk::ToggleButton>("toggle_players");
  toggle_plugins = builder->get_widget<Gtk::ToggleButton>("toggle_plugins");

  popover_blocklist = builder->get_widget<Gtk::Popover>("popover_blocklist");
  blocklist_scrolled_window = builder->get_widget<Gtk::ScrolledWindow>("blocklist_scrolled_window");
  blocklist_player_name = builder->get_widget<Gtk::Text>("blocklist_player_name");
  button_add_to_blocklist = builder->get_widget<Gtk::Button>("button_add_to_blocklist");
  show_blocklisted_apps = builder->get_widget<Gtk::Switch>("show_blocklisted_apps");
  listview_blocklist = builder->get_widget<Gtk::ListView>("listview_blocklist");

  popover_plugins = builder->get_widget<Gtk::Popover>("popover_plugins");
  scrolled_window_plugins = builder->get_widget<Gtk::ScrolledWindow>("scrolled_window_plugins");
  listview_plugins = builder->get_widget<Gtk::ListView>("listview_plugins");
  listview_selected_plugins = builder->get_widget<Gtk::ListView>("listview_selected_plugins");
  entry_plugins_search = builder->get_widget<Gtk::SearchEntry>("entry_plugins_search");
  stack_plugins = builder->get_widget<Gtk::Stack>("stack_plugins");

  stack_top_model = stack_top->get_pages();

  add_plugins_to_stack_plugins();

  // configuring widgets

  setup_listview_players();
  setup_listview_blocklist();
  setup_listview_plugins();
  setup_listview_selected_plugins();

  // spectrum

  auto* box_spectrum = builder->get_widget<Gtk::Box>("box_spectrum");

  spectrum_ui = SpectrumUi::add_to_box(box_spectrum);

  // gsettings

  settings->bind("show-blocklisted-apps", show_blocklisted_apps, "active");

  // signals connections

  stack_top->connect_property_changed("visible-child", [=, this]() {
    auto name = stack_top->get_visible_child_name();

    if (name == "page_players") {
      menubutton_blocklist->set_visible(true);
    } else {
      menubutton_blocklist->set_visible(false);
    }
  });

  stack_top_model->signal_selection_changed().connect([&, this](guint position, guint n_items) {
    toggle_players->set_active(stack_top_model->is_selected(0));
    toggle_plugins->set_active(stack_top_model->is_selected(1));
  });

  toggle_players->signal_toggled().connect([&, this]() {
    if (toggle_players->get_active()) {
      stack_top->get_pages()->select_item(0, true);
    } else {
      toggle_players->set_active(stack_top_model->is_selected(0));
    }
  });

  toggle_plugins->signal_toggled().connect([&, this]() {
    if (toggle_plugins->get_active()) {
      stack_top->get_pages()->select_item(1, true);
    } else {
      toggle_plugins->set_active(stack_top_model->is_selected(1));
    }
  });

  button_add_to_blocklist->signal_clicked().connect([=, this]() {
    if (add_new_blocklist_entry(blocklist_player_name->get_text())) {
      blocklist_player_name->set_text("");
    }
  });

  show_blocklisted_apps->signal_state_set().connect(
      [=, this](bool state) {
        if (state) {
          players_model->remove_all();

          listview_players->set_model(nullptr);

          for (guint n = 0; n < all_players_model->get_n_items(); n++) {
            players_model->append(all_players_model->get_item(n));
          }

          listview_players->set_model(Gtk::NoSelection::create(players_model));
        } else {
          players_model->remove_all();

          listview_players->set_model(nullptr);

          for (guint n = 0; n < all_players_model->get_n_items(); n++) {
            auto item = all_players_model->get_item(n);

            if (!app_is_blocklisted(item->info.name)) {
              players_model->append(item);
            }
          }

          listview_players->set_model(Gtk::NoSelection::create(players_model));
        }

        return false;
      },
      false);

  popover_blocklist->signal_show().connect([=, this]() {
    int height = static_cast<int>(0.5F * static_cast<float>(stack_top->get_allocated_height()));

    blocklist_scrolled_window->set_max_content_height(height);
  });

  popover_plugins->signal_show().connect([=, this]() {
    int height = static_cast<int>(0.5F * static_cast<float>(stack_top->get_allocated_height()));

    scrolled_window_plugins->set_max_content_height(height);
  });

  effects_base->autogain->post_messages = true;
  effects_base->bass_enhancer->post_messages = true;
  effects_base->compressor->post_messages = true;
  effects_base->delay->post_messages = true;
  effects_base->equalizer->post_messages = true;
  effects_base->exciter->post_messages = true;
  effects_base->filter->post_messages = true;
  effects_base->gate->post_messages = true;
  effects_base->limiter->post_messages = true;
  effects_base->maximizer->post_messages = true;
  effects_base->multiband_compressor->post_messages = true;
  effects_base->output_level->post_messages = true;
  effects_base->reverb->post_messages = true;
  effects_base->spectrum->post_messages = true;
  effects_base->stereo_tools->post_messages = true;
}

EffectsBaseUi::~EffectsBaseUi() {
  util::debug("effects_base_ui: destroyed");

  for (auto& c : connections) {
    c.disconnect();
  }

  effects_base->autogain->post_messages = false;
  effects_base->bass_enhancer->post_messages = false;
  effects_base->compressor->post_messages = false;
  effects_base->delay->post_messages = false;
  effects_base->equalizer->post_messages = false;
  effects_base->exciter->post_messages = false;
  effects_base->filter->post_messages = false;
  effects_base->gate->post_messages = false;
  effects_base->limiter->post_messages = false;
  effects_base->maximizer->post_messages = false;
  effects_base->multiband_compressor->post_messages = false;
  effects_base->output_level->post_messages = false;
  effects_base->reverb->post_messages = false;
  effects_base->spectrum->post_messages = false;
  effects_base->stereo_tools->post_messages = false;
}

void EffectsBaseUi::add_plugins_to_stack_plugins() {
  std::string path = "/" + schema + "/";

  std::replace(path.begin(), path.end(), '.', '/');

  auto* autogain_ui = AutoGainUi::add_to_stack(stack_plugins, path);

  autogain_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->autogain->bypass = autogain_ui->bypass->get_active(); });

  effects_base->autogain->input_level.connect(sigc::mem_fun(*autogain_ui, &AutoGainUi::on_new_input_level));
  effects_base->autogain->output_level.connect(sigc::mem_fun(*autogain_ui, &AutoGainUi::on_new_output_level));
  effects_base->autogain->results.connect(sigc::mem_fun(*autogain_ui, &AutoGainUi::on_new_results));

  // bass enhancer

  auto* bass_enhancer_ui = BassEnhancerUi::add_to_stack(stack_plugins, path);

  bass_enhancer_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->bass_enhancer->bypass = bass_enhancer_ui->bypass->get_active(); });

  effects_base->bass_enhancer->input_level.connect(
      sigc::mem_fun(*bass_enhancer_ui, &BassEnhancerUi::on_new_input_level));
  effects_base->bass_enhancer->output_level.connect(
      sigc::mem_fun(*bass_enhancer_ui, &BassEnhancerUi::on_new_output_level));
  effects_base->bass_enhancer->harmonics.connect(
      sigc::mem_fun(*bass_enhancer_ui, &BassEnhancerUi::on_new_harmonics_level));

  // compressor

  auto* compressor_ui = CompressorUi::add_to_stack(stack_plugins, path);

  compressor_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->compressor->bypass = compressor_ui->bypass->get_active(); });

  effects_base->compressor->input_level.connect(sigc::mem_fun(*compressor_ui, &CompressorUi::on_new_input_level));
  effects_base->compressor->output_level.connect(sigc::mem_fun(*compressor_ui, &CompressorUi::on_new_output_level));
  effects_base->compressor->reduction.connect(sigc::mem_fun(*compressor_ui, &CompressorUi::on_new_reduction));
  effects_base->compressor->sidechain.connect(sigc::mem_fun(*compressor_ui, &CompressorUi::on_new_sidechain));
  effects_base->compressor->curve.connect(sigc::mem_fun(*compressor_ui, &CompressorUi::on_new_curve));

  // delay

  auto* delay_ui = DelayUi::add_to_stack(stack_plugins, path);

  delay_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->delay->bypass = delay_ui->bypass->get_active(); });

  effects_base->delay->input_level.connect(sigc::mem_fun(*delay_ui, &DelayUi::on_new_input_level));
  effects_base->delay->output_level.connect(sigc::mem_fun(*delay_ui, &DelayUi::on_new_output_level));

  // equalizer

  auto* equalizer_ui = EqualizerUi::add_to_stack(stack_plugins, path);

  equalizer_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->equalizer->bypass = equalizer_ui->bypass->get_active(); });

  effects_base->equalizer->input_level.connect(sigc::mem_fun(*equalizer_ui, &EqualizerUi::on_new_input_level));
  effects_base->equalizer->output_level.connect(sigc::mem_fun(*equalizer_ui, &EqualizerUi::on_new_output_level));

  // exciter

  auto* exciter_ui = ExciterUi::add_to_stack(stack_plugins, path);

  exciter_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->exciter->bypass = exciter_ui->bypass->get_active(); });

  effects_base->exciter->input_level.connect(sigc::mem_fun(*exciter_ui, &ExciterUi::on_new_input_level));
  effects_base->exciter->output_level.connect(sigc::mem_fun(*exciter_ui, &ExciterUi::on_new_output_level));
  effects_base->exciter->harmonics.connect(sigc::mem_fun(*exciter_ui, &ExciterUi::on_new_harmonics_level));

  // filter

  auto* filter_ui = FilterUi::add_to_stack(stack_plugins, path);

  filter_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->filter->bypass = filter_ui->bypass->get_active(); });

  effects_base->filter->input_level.connect(sigc::mem_fun(*filter_ui, &FilterUi::on_new_input_level));
  effects_base->filter->output_level.connect(sigc::mem_fun(*filter_ui, &FilterUi::on_new_output_level));

  // gate

  auto* gate_ui = GateUi::add_to_stack(stack_plugins, path);

  gate_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->gate->bypass = gate_ui->bypass->get_active(); });

  effects_base->gate->input_level.connect(sigc::mem_fun(*gate_ui, &GateUi::on_new_input_level));
  effects_base->gate->output_level.connect(sigc::mem_fun(*gate_ui, &GateUi::on_new_output_level));
  effects_base->gate->gating.connect(sigc::mem_fun(*gate_ui, &GateUi::on_new_gating));

  // limiter

  auto* limiter_ui = LimiterUi::add_to_stack(stack_plugins, path);

  limiter_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->limiter->bypass = limiter_ui->bypass->get_active(); });

  effects_base->limiter->input_level.connect(sigc::mem_fun(*limiter_ui, &LimiterUi::on_new_input_level));
  effects_base->limiter->output_level.connect(sigc::mem_fun(*limiter_ui, &LimiterUi::on_new_output_level));
  effects_base->limiter->attenuation.connect(sigc::mem_fun(*limiter_ui, &LimiterUi::on_new_attenuation));

  // maximizer

  auto* maximizer_ui = MaximizerUi::add_to_stack(stack_plugins, path);

  maximizer_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->maximizer->bypass = maximizer_ui->bypass->get_active(); });

  effects_base->maximizer->input_level.connect(sigc::mem_fun(*maximizer_ui, &MaximizerUi::on_new_input_level));
  effects_base->maximizer->output_level.connect(sigc::mem_fun(*maximizer_ui, &MaximizerUi::on_new_output_level));
  effects_base->maximizer->reduction.connect(sigc::mem_fun(*maximizer_ui, &MaximizerUi::on_new_reduction));

  // multiband compressor

  auto* multiband_compressor_ui = MultibandCompressorUi::add_to_stack(stack_plugins, path);

  multiband_compressor_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->multiband_compressor->bypass = multiband_compressor_ui->bypass->get_active(); });

  effects_base->multiband_compressor->input_level.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_input_level));
  effects_base->multiband_compressor->output_level.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_output_level));

  effects_base->multiband_compressor->output0.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_output0));
  effects_base->multiband_compressor->output1.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_output1));
  effects_base->multiband_compressor->output2.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_output2));
  effects_base->multiband_compressor->output3.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_output3));

  effects_base->multiband_compressor->compression0.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_compression0));
  effects_base->multiband_compressor->compression1.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_compression1));
  effects_base->multiband_compressor->compression2.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_compression2));
  effects_base->multiband_compressor->compression3.connect(
      sigc::mem_fun(*multiband_compressor_ui, &MultibandCompressorUi::on_new_compression3));

  // reverb

  auto* reverb_ui = ReverbUi::add_to_stack(stack_plugins, path);

  reverb_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->reverb->bypass = reverb_ui->bypass->get_active(); });

  effects_base->reverb->input_level.connect(sigc::mem_fun(*reverb_ui, &ReverbUi::on_new_input_level));
  effects_base->reverb->output_level.connect(sigc::mem_fun(*reverb_ui, &ReverbUi::on_new_output_level));

  // stereo tools

  auto* stereo_tools_ui = StereoToolsUi::add_to_stack(stack_plugins, path);

  stereo_tools_ui->bypass->signal_toggled().connect(
      [=, this]() { effects_base->stereo_tools->bypass = stereo_tools_ui->bypass->get_active(); });

  effects_base->stereo_tools->input_level.connect(sigc::mem_fun(*stereo_tools_ui, &StereoToolsUi::on_new_input_level));
  effects_base->stereo_tools->output_level.connect(
      sigc::mem_fun(*stereo_tools_ui, &StereoToolsUi::on_new_output_level));
  effects_base->stereo_tools->new_correlation.connect(
      sigc::mem_fun(*stereo_tools_ui, &StereoToolsUi::on_new_phase_correlation));
}

void EffectsBaseUi::setup_listview_players() {
  // setting the listview model and factory

  listview_players->set_model(Gtk::NoSelection::create(players_model));

  auto factory = Gtk::SignalListItemFactory::create();

  listview_players->set_factory(factory);

  // setting the factory callbacks

  factory->signal_setup().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto b = Gtk::Builder::create_from_resource("/com/github/wwmm/pulseeffects/ui/app_info.ui");

    auto* top_box = b->get_widget<Gtk::Box>("top_box");

    list_item->set_data("enable", b->get_widget<Gtk::Switch>("enable"));
    list_item->set_data("app_icon", b->get_widget<Gtk::Image>("app_icon"));
    list_item->set_data("app_name", b->get_widget<Gtk::Label>("app_name"));
    list_item->set_data("media_name", b->get_widget<Gtk::Label>("media_name"));
    list_item->set_data("blocklist", b->get_widget<Gtk::CheckButton>("blocklist"));
    list_item->set_data("format", b->get_widget<Gtk::Label>("format"));
    list_item->set_data("rate", b->get_widget<Gtk::Label>("rate"));
    list_item->set_data("channels", b->get_widget<Gtk::Label>("channels"));
    list_item->set_data("latency", b->get_widget<Gtk::Label>("latency"));
    list_item->set_data("state", b->get_widget<Gtk::Label>("state"));
    list_item->set_data("mute", b->get_widget<Gtk::ToggleButton>("mute"));
    list_item->set_data("scale_volume", b->get_widget<Gtk::Scale>("scale_volume"));
    list_item->set_data("volume", b->get_object<Gtk::Adjustment>("volume").get());

    list_item->set_child(*top_box);
  });

  factory->signal_bind().connect([=, this](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* app_name = static_cast<Gtk::Label*>(list_item->get_data("app_name"));
    auto* media_name = static_cast<Gtk::Label*>(list_item->get_data("media_name"));
    auto* format = static_cast<Gtk::Label*>(list_item->get_data("format"));
    auto* rate = static_cast<Gtk::Label*>(list_item->get_data("rate"));
    auto* channels = static_cast<Gtk::Label*>(list_item->get_data("channels"));
    auto* latency = static_cast<Gtk::Label*>(list_item->get_data("latency"));
    auto* state = static_cast<Gtk::Label*>(list_item->get_data("state"));
    auto* enable = static_cast<Gtk::Switch*>(list_item->get_data("enable"));
    auto* app_icon = static_cast<Gtk::Image*>(list_item->get_data("app_icon"));
    auto* scale_volume = static_cast<Gtk::Scale*>(list_item->get_data("scale_volume"));
    auto* volume = static_cast<Gtk::Adjustment*>(list_item->get_data("volume"));
    auto* mute = static_cast<Gtk::ToggleButton*>(list_item->get_data("mute"));
    auto* blocklist = static_cast<Gtk::CheckButton*>(list_item->get_data("blocklist"));

    auto holder = std::dynamic_pointer_cast<NodeInfoHolder>(list_item->get_item());

    auto connection_enable = enable->signal_state_set().connect(
        [=, this](bool state) {
          if (state) {
            if (holder->info.media_class == "Stream/Output/Audio") {
              pm->connect_stream_output(holder->info);
            } else if (holder->info.media_class == "Stream/Input/Audio") {
              pm->connect_stream_input(holder->info);
            }
          } else {
            if (holder->info.media_class == "Stream/Output/Audio") {
              pm->disconnect_stream_output(holder->info);
            } else if (holder->info.media_class == "Stream/Input/Audio") {
              pm->disconnect_stream_input(holder->info);
            }
          }
          return false;
        },
        false);

    auto connection_volume = volume->signal_value_changed().connect(
        [=]() { PipeManager::set_node_volume(holder->info, static_cast<float>(volume->get_value()) / 100.0F); });

    auto connection_mute = mute->signal_toggled().connect([=]() {
      bool state = mute->get_active();

      if (state) {
        mute->property_icon_name().set_value("audio-volume-muted-symbolic");

        scale_volume->set_sensitive(false);
      } else {
        mute->property_icon_name().set_value("audio-volume-high-symbolic");

        scale_volume->set_sensitive(true);
      }

      PipeManager::set_node_mute(holder->info, state);
    });

    auto connection_blocklist = blocklist->signal_toggled().connect([=, this]() {
      if (blocklist->get_active()) {
        add_new_blocklist_entry(holder->info.name);

        enable->set_active(false);

        enable->set_sensitive(false);
      } else {
        remove_blocklist_entry(holder->info.name);

        enable->set_sensitive(true);
      }
    });

    auto* pointer_connection_enable = new sigc::connection(connection_enable);
    auto* pointer_connection_volume = new sigc::connection(connection_volume);
    auto* pointer_connection_mute = new sigc::connection(connection_mute);
    auto* pointer_connection_blocklist = new sigc::connection(connection_blocklist);

    auto connection_info = holder->info_updated.connect([=, this](const NodeInfo& i) {
      app_name->set_text(i.name);
      media_name->set_text(i.media_name);
      format->set_text(i.format);
      rate->set_text(std::to_string(i.rate) + " Hz");
      channels->set_text(std::to_string(i.n_volume_channels));
      latency->set_text(float_to_localized_string(i.latency, 2) + " s");

      if (!i.app_icon_name.empty()) {
        app_icon->set_from_icon_name(i.app_icon_name);
      } else if (!i.media_icon_name.empty()) {
        app_icon->set_from_icon_name(i.media_icon_name);
      } else {
        auto str = i.name;

        // We need this to make Firefox icon visible =/

        std::transform(str.begin(), str.end(), str.begin(), ::tolower);

        app_icon->set_from_icon_name(str);
      }

      switch (i.state) {
        case PW_NODE_STATE_RUNNING:
          state->set_text(_("running"));

          break;
        case PW_NODE_STATE_SUSPENDED:
          state->set_text(_("suspended"));

          break;
        case PW_NODE_STATE_IDLE:
          state->set_text(_("idle"));

          break;
        case PW_NODE_STATE_CREATING:
          state->set_text(_("creating"));

          break;
        case PW_NODE_STATE_ERROR:
          state->set_text(_("error"));

          break;
        default:
          break;
      }

      // initializing the switch

      pointer_connection_enable->block();

      bool is_enabled = false;

      if (holder->info.media_class == "Stream/Output/Audio") {
        for (const auto& link : pm->list_links) {
          if (link.output_node_id == holder->info.id && link.input_node_id == pm->pe_sink_node.id) {
            is_enabled = true;

            break;
          }
        }
      } else if (holder->info.media_class == "Stream/Input/Audio") {
        for (const auto& link : pm->list_links) {
          if (link.output_node_id == pm->pe_source_node.id && link.input_node_id == holder->info.id) {
            is_enabled = true;

            break;
          }
        }
      }

      bool is_blocklisted = app_is_blocklisted(holder->info.name);

      enable->set_active(is_enabled);
      enable->set_active(is_enabled && !is_blocklisted);
      enable->set_sensitive(!is_blocklisted);

      pointer_connection_enable->unblock();

      // initializing the volume slide

      pointer_connection_volume->block();

      volume->set_value(100 * holder->info.volume);

      pointer_connection_volume->unblock();

      // initializing the mute button

      pointer_connection_mute->block();

      if (holder->info.mute) {
        mute->property_icon_name().set_value("audio-volume-muted-symbolic");

        scale_volume->set_sensitive(false);
      } else {
        mute->property_icon_name().set_value("audio-volume-high-symbolic");

        scale_volume->set_sensitive(true);
      }

      mute->set_active(holder->info.mute);

      pointer_connection_mute->unblock();

      // initializing the blocklist checkbutton

      pointer_connection_blocklist->block();

      blocklist->set_active(is_blocklisted);

      pointer_connection_blocklist->unblock();
    });

    scale_volume->set_format_value_func([=](double v) { return std::to_string(static_cast<int>(v)) + " %"; });

    holder->info_updated.emit(holder->info);

    list_item->set_data("connection_enable", pointer_connection_enable, Glib::destroy_notify_delete<sigc::connection>);

    list_item->set_data("connection_volume", pointer_connection_volume, Glib::destroy_notify_delete<sigc::connection>);

    list_item->set_data("connection_mute", pointer_connection_mute, Glib::destroy_notify_delete<sigc::connection>);

    list_item->set_data("connection_blocklist", pointer_connection_blocklist,
                        Glib::destroy_notify_delete<sigc::connection>);

    list_item->set_data("connection_info", new sigc::connection(connection_info),
                        Glib::destroy_notify_delete<sigc::connection>);
  });

  factory->signal_unbind().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    for (const auto* conn :
         {"connection_enable", "connection_volume", "connection_mute", "connection_blocklist", "connection_info"}) {
      if (auto* connection = static_cast<sigc::connection*>(list_item->get_data(conn))) {
        connection->disconnect();

        list_item->set_data(conn, nullptr);
      }
    }
  });
}

void EffectsBaseUi::setup_listview_blocklist() {
  blocklist->remove(0);

  for (auto& name : settings->get_string_array("blocklist")) {
    blocklist->append(name);
  }

  settings->signal_changed("blocklist").connect([=, this](auto key) {
    auto list = settings->get_string_array(key);

    blocklist->splice(0, blocklist->get_n_items(), list);
  });

  blocklist->signal_items_changed().connect([=, this](guint position, guint removed, guint added) {
    if (removed > 0) {
      players_model->remove_all();

      listview_players->set_model(nullptr);

      for (guint n = 0; n < all_players_model->get_n_items(); n++) {
        players_model->append(all_players_model->get_item(n));
      }

      listview_players->set_model(Gtk::NoSelection::create(players_model));
    }
  });

  // sorter

  auto sorter =
      Gtk::StringSorter::create(Gtk::PropertyExpression<Glib::ustring>::create(GTK_TYPE_STRING_OBJECT, "string"));

  auto sort_list_model = Gtk::SortListModel::create(blocklist, sorter);

  // setting the listview model and factory

  listview_blocklist->set_model(Gtk::NoSelection::create(sort_list_model));

  auto factory = Gtk::SignalListItemFactory::create();

  listview_blocklist->set_factory(factory);

  // setting the factory callbacks

  factory->signal_setup().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* box = Gtk::make_managed<Gtk::Box>();
    auto* label = Gtk::make_managed<Gtk::Label>();
    auto* btn = Gtk::make_managed<Gtk::Button>();

    label->set_hexpand(true);
    label->set_halign(Gtk::Align::START);

    btn->set_icon_name("list-remove-symbolic");

    box->append(*label);
    box->append(*btn);

    list_item->set_data("name", label);
    list_item->set_data("remove", btn);

    list_item->set_child(*box);
  });

  factory->signal_bind().connect([=, this](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* label = static_cast<Gtk::Label*>(list_item->get_data("name"));
    auto* remove = static_cast<Gtk::Button*>(list_item->get_data("remove"));

    auto name = list_item->get_item()->get_property<Glib::ustring>("string");

    label->set_text(name);

    auto connection_remove = remove->signal_clicked().connect([=, this]() {
      auto list = settings->get_string_array("blocklist");

      list.erase(std::remove_if(list.begin(), list.end(), [=](auto& player_name) { return player_name == name; }),
                 list.end());

      settings->set_string_array("blocklist", list);
    });

    list_item->set_data("connection_remove", new sigc::connection(connection_remove),
                        Glib::destroy_notify_delete<sigc::connection>);
  });

  factory->signal_unbind().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    if (auto* connection = static_cast<sigc::connection*>(list_item->get_data("connection_remove"))) {
      connection->disconnect();

      list_item->set_data("connection_remove", nullptr);
    }
  });
}

void EffectsBaseUi::setup_listview_plugins() {
  plugins->remove(0);

  for (auto& name : settings->get_string_array("plugins")) {
    plugins->append(name);
  }

  settings->signal_changed("plugins").connect([=, this](auto key) {
    auto list = settings->get_string_array(key);

    plugins->splice(0, plugins->get_n_items(), list);
  });

  // filter

  auto filter =
      Gtk::StringFilter::create(Gtk::PropertyExpression<Glib::ustring>::create(GTK_TYPE_STRING_OBJECT, "string"));

  auto filter_model = Gtk::FilterListModel::create(plugins, filter);

  filter_model->set_incremental(true);

  Glib::Binding::bind_property(entry_plugins_search->property_text(), filter->property_search());

  // sorter

  auto sorter =
      Gtk::StringSorter::create(Gtk::PropertyExpression<Glib::ustring>::create(GTK_TYPE_STRING_OBJECT, "string"));

  auto sort_list_model = Gtk::SortListModel::create(filter_model, sorter);

  // setting the listview model and factory

  listview_plugins->set_model(Gtk::NoSelection::create(sort_list_model));

  auto factory = Gtk::SignalListItemFactory::create();

  listview_plugins->set_factory(factory);

  // setting the factory callbacks

  factory->signal_setup().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* box = Gtk::make_managed<Gtk::Box>();
    auto* label = Gtk::make_managed<Gtk::Label>();
    auto* btn = Gtk::make_managed<Gtk::Button>();

    label->set_hexpand(true);
    label->set_halign(Gtk::Align::START);

    btn->set_icon_name("list-add-symbolic");

    box->append(*label);
    box->append(*btn);

    list_item->set_data("name", label);
    list_item->set_data("add", btn);

    list_item->set_child(*box);
  });

  factory->signal_bind().connect([=, this](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* label = static_cast<Gtk::Label*>(list_item->get_data("name"));
    auto* add = static_cast<Gtk::Button*>(list_item->get_data("add"));

    auto name = list_item->get_item()->get_property<Glib::ustring>("string");

    label->set_text(plugins_names[name]);

    auto connection_add = add->signal_clicked().connect([=, this]() {
      auto list = settings->get_string_array("selected-plugins");

      if (std::ranges::find(list, name) == list.end()) {
        list.emplace_back(name);

        settings->set_string_array("selected-plugins", list);
      }
    });

    list_item->set_data("connection_add", new sigc::connection(connection_add),
                        Glib::destroy_notify_delete<sigc::connection>);
  });

  factory->signal_unbind().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    if (auto* connection = static_cast<sigc::connection*>(list_item->get_data("connection_add"))) {
      connection->disconnect();

      list_item->set_data("connection_add", nullptr);
    }
  });
}

void EffectsBaseUi::setup_listview_selected_plugins() {
  selected_plugins->remove(0);

  if (settings->get_string_array("selected-plugins").empty()) {
    stack_plugins->property_visible().set_value(false);
  } else {
    for (auto& name : settings->get_string_array("selected-plugins")) {
      selected_plugins->append(name);
    }
  }

  settings->signal_changed("selected-plugins").connect([=, this](auto key) {
    auto list = settings->get_string_array(key);

    selected_plugins->splice(0, selected_plugins->get_n_items(), list);

    if (!list.empty()) {
      auto visible_page_name = stack_plugins->get_page(*stack_plugins->get_visible_child())->get_name();

      if (std::ranges::find(list, visible_page_name) == list.end()) {
        listview_selected_plugins->get_model()->select_item(0, true);
        listview_selected_plugins->get_first_child()->activate();
      }

      stack_plugins->property_visible().set_value(true);
    } else {
      stack_plugins->property_visible().set_value(false);
    }
  });

  // setting the listview model and factory

  listview_selected_plugins->set_model(Gtk::SingleSelection::create(selected_plugins));

  auto factory = Gtk::SignalListItemFactory::create();

  listview_selected_plugins->set_factory(factory);

  // setting the item selection callback

  listview_selected_plugins->get_model()->signal_selection_changed().connect([&](guint position, guint n_items) {
    for (guint n = position; n < position + n_items; n++) {
      bool is_selected = listview_selected_plugins->get_model()->is_selected(n);

      if (is_selected) {
        auto selected_name = selected_plugins->get_string(n);

        for (auto* child = stack_plugins->get_first_child(); child != nullptr; child = child->get_next_sibling()) {
          auto page = stack_plugins->get_page(*child);

          if (page->get_name() == selected_name) {
            stack_plugins->set_visible_child(*child);

            return;
          }
        }
      }
    }
  });

  // setting the factory callbacks

  factory->signal_setup().connect([=, this](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* box = Gtk::make_managed<Gtk::Box>();
    auto* label = Gtk::make_managed<Gtk::Label>();
    auto* remove = Gtk::make_managed<Gtk::Button>();
    auto* drag_handle = Gtk::make_managed<Gtk::Image>();

    auto remove_css_classes = remove->get_css_classes();

    remove_css_classes.emplace_back("flat");

    label->set_hexpand(true);
    label->set_halign(Gtk::Align::START);

    remove->set_icon_name("user-trash-symbolic");
    remove->set_css_classes(remove_css_classes);

    drag_handle->set_from_icon_name("list-drag-handle-symbolic");

    box->set_spacing(6);
    box->append(*label);
    box->append(*remove);
    box->append(*drag_handle);

    auto drag_source = Gtk::DragSource::create();
    auto drop_target = Gtk::DropTarget::create(Glib::Value<Glib::ustring>::value_type(), Gdk::DragAction::MOVE);

    drag_source->set_actions(Gdk::DragAction::MOVE);

    drag_source->signal_prepare().connect(
        [=](const double& x, const double& y) {
          auto* controller_widget = drag_source->get_widget();

          auto* item = controller_widget->get_ancestor(Gtk::Box::get_type());

          controller_widget->set_data("dragged-item", item);

          Glib::Value<Glib::RefPtr<const Gtk::Label>> texture_value;

          Glib::Value<Glib::ustring> name_value;

          name_value.init(Glib::Value<Glib::ustring>::value_type());

          name_value.set(label->get_name());

          return Gdk::ContentProvider::create(name_value);
        },
        false);

    drag_source->signal_drag_begin().connect([=](const Glib::RefPtr<Gdk::Drag>& drag) {
      auto* controller_widget = drag_source->get_widget();

      auto* row_box = static_cast<Gtk::Box*>(controller_widget->get_data("dragged-item"));

      auto paintable = Gtk::WidgetPaintable::create(*row_box);

      drag_source->set_icon(paintable, row_box->get_allocated_width() - controller_widget->get_allocated_width() / 2,
                            row_box->get_allocated_height() / 2);

      controller_widget->set_data("dragged-item", nullptr);
    });

    drop_target->signal_drop().connect(
        [=, this](const Glib::ValueBase& v, const double& x, const double& y) {
          Glib::Value<Glib::ustring> name_value;

          name_value.init(v.gobj());

          auto src = name_value.get();
          auto dst = label->get_name();

          if (src != dst) {
            auto list = settings->get_string_array("selected-plugins");

            auto iter_src = std::ranges::find(list, src);
            auto iter_dst = std::ranges::find(list, dst);

            auto insert_after = (iter_src - list.begin() < iter_dst - list.begin()) ? true : false;

            list.erase(iter_src);

            iter_dst = std::ranges::find(list, dst);

            if (insert_after) {
              list.insert(iter_dst + 1, src);
            } else {
              list.insert(iter_dst, src);
            }

            settings->set_string_array("selected-plugins", list);
          }

          return true;
        },
        false);

    drag_handle->add_controller(drag_source);
    box->add_controller(drop_target);

    // setting list_item data

    list_item->set_data("name", label);
    list_item->set_data("remove", remove);

    list_item->set_child(*box);
  });

  factory->signal_bind().connect([=, this](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    auto* label = static_cast<Gtk::Label*>(list_item->get_data("name"));
    auto* remove = static_cast<Gtk::Button*>(list_item->get_data("remove"));

    auto name = list_item->get_item()->get_property<Glib::ustring>("string");

    label->set_name(name);
    label->set_text(plugins_names[name]);

    auto connection_remove = remove->signal_clicked().connect([=, this]() {
      auto list = settings->get_string_array("selected-plugins");

      list.erase(std::remove_if(list.begin(), list.end(), [=](auto& plugin_name) { return plugin_name == name; }),
                 list.end());

      settings->set_string_array("selected-plugins", list);
    });

    // setting list_item data

    list_item->set_data("connection_remove", new sigc::connection(connection_remove),
                        Glib::destroy_notify_delete<sigc::connection>);
  });

  factory->signal_unbind().connect([=](const Glib::RefPtr<Gtk::ListItem>& list_item) {
    for (const auto* conn : {"connection_remove"}) {
      if (auto* connection = static_cast<sigc::connection*>(list_item->get_data(conn))) {
        connection->disconnect();

        list_item->set_data(conn, nullptr);
      }
    }
  });
}

void EffectsBaseUi::on_app_added(NodeInfo node_info) {
  // do not add the same stream twice

  for (guint n = 0; n < all_players_model->get_n_items(); n++) {
    auto item = all_players_model->get_item(n);

    if (item->info.id == node_info.id) {
      return;
    }
  }

  all_players_model->append(NodeInfoHolder::create(node_info));

  // Blocklist check

  auto forbidden_app = app_is_blocklisted(node_info.name);

  if (forbidden_app) {
    if (!settings->get_boolean("show-blocklisted-apps")) {
      return;
    }
  }

  players_model->append(NodeInfoHolder::create(node_info));
}

void EffectsBaseUi::on_app_changed(NodeInfo node_info) {
  for (guint n = 0; n < players_model->get_n_items(); n++) {
    auto* item = players_model->get_item(n).get();

    if (item->info.id == node_info.id) {
      item->info = node_info;
      item->info_updated.emit(node_info);

      break;
    }
  }
}

void EffectsBaseUi::on_app_removed(NodeInfo node_info) {
  for (guint n = 0; n < players_model->get_n_items(); n++) {
    auto item = players_model->get_item(n);

    if (item->info.id == node_info.id) {
      players_model->remove(n);

      break;
    }
  }

  for (guint n = 0; n < all_players_model->get_n_items(); n++) {
    auto item = all_players_model->get_item(n);

    if (item->info.id == node_info.id) {
      all_players_model->remove(n);

      break;
    }
  }
}

void EffectsBaseUi::on_new_output_level_db(const float& left, const float& right) {
  global_output_level_left->set_text(level_to_localized_string_showpos(left, 0));

  global_output_level_right->set_text(level_to_localized_string_showpos(right, 0));

  // saturation icon notification

  if (left > 0.0 || right > 0.0) {
    saturation_icon->set_visible(true);
  } else {
    saturation_icon->set_visible(false);
  }
}

auto EffectsBaseUi::node_state_to_string(const pw_node_state& state) -> std::string {
  switch (state) {
    case PW_NODE_STATE_RUNNING:
      return _("running");
    case PW_NODE_STATE_SUSPENDED:
      return _("suspended");
    case PW_NODE_STATE_IDLE:
      return _("idle");
    case PW_NODE_STATE_CREATING:
      return _("creating");
    case PW_NODE_STATE_ERROR:
      return _("error");
    default:
      return "";
  }
}

auto EffectsBaseUi::float_to_localized_string(const float& value, const int& places) -> std::string {
  std::ostringstream msg;

  msg.precision(places);

  msg << std::fixed << value;

  return msg.str();
}

auto EffectsBaseUi::app_is_blocklisted(const Glib::ustring& name) -> bool {
  std::vector<Glib::ustring> bl = settings->get_string_array("blocklist");

  return std::ranges::find(bl, name) != bl.end();
}

auto EffectsBaseUi::add_new_blocklist_entry(const Glib::ustring& name) -> bool {
  if (name.empty()) {
    return false;
  }

  std::vector<Glib::ustring> bl = settings->get_string_array("blocklist");

  if (std::any_of(bl.cbegin(), bl.cend(), [&](auto str) { return str == name; })) {
    util::debug("blocklist_settings_ui: entry already present in the list");

    return false;
  }

  bl.emplace_back(name);

  settings->set_string_array("blocklist", bl);

  util::debug("blocklist_settings_ui: new entry has been added to the blocklist");

  return true;
}

void EffectsBaseUi::remove_blocklist_entry(const Glib::ustring& name) {
  std::vector<Glib::ustring> bl = settings->get_string_array("blocklist");

  bl.erase(std::remove_if(bl.begin(), bl.end(), [=](auto& a) { return a == name; }), bl.end());

  settings->set_string_array("blocklist", bl);

  util::debug("blocklist_settings_ui: an entry has been removed from the blocklist");
}
