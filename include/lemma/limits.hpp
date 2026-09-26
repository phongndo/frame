#ifndef LEMMA_LIMITS_HPP
#define LEMMA_LIMITS_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace lemma::limits {

inline constexpr std::uint32_t sessions_hard_max = 64;
inline constexpr std::uint32_t tabs_hard_max = 1'024;
inline constexpr std::uint32_t panes_hard_max = 4'096;
inline constexpr std::uint32_t clients_hard_max = 128;
inline constexpr std::uint32_t layout_depth_hard_max = 64;

// Runtime extensions are isolated peers. These limits bound both retained daemon state and work
// admitted from continuously ready peers; they are part of the discoverable extension contract.
inline constexpr std::size_t extension_sessions_hard_max = 32;
inline constexpr std::size_t extension_surfaces_per_owner_max = 16;
inline constexpr std::size_t extension_surfaces_hard_max = 128;
inline constexpr std::size_t extension_procs_per_owner_max = 1;
inline constexpr std::size_t extension_record_header_bytes = 16;
inline constexpr std::size_t extension_record_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t extension_io_bytes_per_turn_max = std::size_t{16} * 1'024U;
inline constexpr std::size_t extension_records_per_turn_max = 16;
inline constexpr std::size_t extension_bytes_per_turn_max = std::size_t{256} * 1'024U;
inline constexpr std::size_t extension_record_work_bytes_per_turn_max =
    extension_record_bytes_max + (extension_records_per_turn_max * extension_record_header_bytes);
inline constexpr std::size_t extension_output_bytes_per_turn_max =
    extension_sessions_hard_max * extension_io_bytes_per_turn_max;
inline constexpr std::size_t extension_output_bytes_per_owner_max =
    std::size_t{2} * 1'024U * 1'024U;
inline constexpr std::size_t extension_input_bytes_max = std::size_t{8} * 1'024U;
inline constexpr std::size_t extension_interaction_events_max = 64;
inline constexpr std::size_t extension_interaction_bytes_per_owner_max =
    std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t extension_input_bytes_aggregate_max =
    extension_sessions_hard_max * (extension_record_bytes_max + extension_record_header_bytes);
inline constexpr std::size_t extension_output_bytes_aggregate_max =
    extension_sessions_hard_max * extension_output_bytes_per_owner_max;
inline constexpr std::size_t surface_styles_max = 64;
inline constexpr std::size_t surface_runs_per_row_max = 256;
inline constexpr std::size_t surface_text_bytes_per_row_max = std::size_t{16} * 1'024U;
inline constexpr std::size_t surface_retained_bytes_max = std::size_t{4} * 1'024U * 1'024U;
inline constexpr std::size_t surface_retained_bytes_aggregate_max =
    std::size_t{64} * 1'024U * 1'024U;

// Session launch semantics own these bounds. Protocol codecs reuse them rather than defining the
// semantic model's storage limits.
inline constexpr std::size_t session_name_bytes_max = 32;
inline constexpr std::size_t tab_title_bytes_max = 64;
inline constexpr std::size_t working_directory_bytes_max = std::size_t{4} * 1'024U;
inline constexpr std::size_t environment_bytes_max = 65'535;
inline constexpr std::size_t environment_entries_max = 256;
inline constexpr std::size_t command_bytes_hard_max = 65'535;
inline constexpr std::size_t command_arguments_hard_max = 256;
inline constexpr std::size_t hosted_commands_hard_max = 64;
inline constexpr std::size_t command_line_bytes_max = 512;
inline constexpr std::size_t command_line_history_max = 16;
inline constexpr std::size_t status_message_bytes_max = 256;
inline constexpr std::size_t status_message_history_max = 16;
inline constexpr auto status_message_display_time = std::chrono::milliseconds{1'500};

// Presentation transaction limits. A frame can span chunks, but queued bytes never retain a
// Ghostty RenderState snapshot while waiting for socket writability.
inline constexpr std::size_t frame_chunk_bytes_max = std::size_t{4} * 1'024U * 1'024U;
inline constexpr std::size_t frame_transaction_bytes_max = std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t frame_output_queue_bytes_max = std::size_t{8} * 1'024U * 1'024U;
// Sanitized outer window title, and its OSC 2 framing (`ESC ] 2 ;` ... `ESC \`) within a frame.
inline constexpr std::size_t outer_title_bytes_max = 256;
inline constexpr std::size_t outer_title_frame_bytes_max = outer_title_bytes_max + 6U;
// Forwarded outer-terminal notification fields and OSC 7 working-directory URI.
inline constexpr std::size_t outer_notification_title_bytes_max = 256;
inline constexpr std::size_t outer_notification_body_bytes_max = 1'024;
inline constexpr std::size_t outer_cwd_bytes_max = 2'048;
// Longest Pane OSC 8 hyperlink URI re-emitted to the outer terminal; longer links are not
// forwarded.
inline constexpr std::size_t outer_hyperlink_uri_bytes_max = 2'048;
// Attention appended to one frame after composition: BEL; one notification
// (`ESC ] 777 ; notify ;` TITLE `;` BODY `ESC \`); progress (at most `ESC ] 9 ; 4 ; 1 ; 100 ESC
// \`); and a working directory (`ESC ] 7 ;` URI `ESC \`).
inline constexpr std::size_t outer_notification_frame_bytes_max =
    13U + outer_notification_title_bytes_max + 1U + outer_notification_body_bytes_max + 2U;
inline constexpr std::size_t outer_progress_frame_bytes_max = 13;
inline constexpr std::size_t outer_cwd_frame_bytes_max = outer_cwd_bytes_max + 6U;
inline constexpr std::size_t outer_attention_frame_bytes_max =
    1U + outer_notification_frame_bytes_max + outer_progress_frame_bytes_max +
    outer_cwd_frame_bytes_max;
// Retained frame storage is shared across all attached and pending-attached sessions. This bound
// admits several protocol-maximum viewports while preventing the session limit from multiplying
// the per-frame transaction ceiling into multi-gigabyte daemon retention.
inline constexpr std::size_t frame_retained_bytes_aggregate_max =
    std::size_t{256} * 1'024U * 1'024U;
inline constexpr auto render_snapshot_hold_max = std::chrono::milliseconds{50};
inline constexpr auto frame_transaction_progress_deadline = std::chrono::seconds{5};
inline constexpr auto frame_transaction_total_deadline = std::chrono::seconds{30};
inline constexpr auto synchronized_output_presentation_timeout = std::chrono::seconds{1};
static_assert(frame_chunk_bytes_max <= frame_output_queue_bytes_max);
static_assert(frame_output_queue_bytes_max < frame_transaction_bytes_max);
static_assert(frame_transaction_bytes_max <= frame_retained_bytes_aggregate_max);

inline constexpr std::size_t client_queue_bytes_hard_max = frame_output_queue_bytes_max;

// Expanded semantic input and child-controlled data limits. Protocol envelopes validate these
// independently so legacy input remains small while opaque typed paste can use its larger bound.
inline constexpr std::size_t structured_input_payload_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t structured_event_batch_expanded_max = 4'096;
inline constexpr std::size_t pixel_mouse_report_bytes_max = 128;
inline constexpr std::size_t paste_payload_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t terminal_effect_text_bytes_max = std::size_t{4} * 1'024U;
inline constexpr std::size_t clipboard_decoded_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t clipboard_response_bytes_max =
    (std::size_t{2} * 1'024U * 1'024U) + 8192U;
inline constexpr std::size_t unknown_sequence_bytes_max = std::size_t{4} * 1'024U;
inline constexpr std::size_t snapshot_bytes_max = std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t snapshot_continuation_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t graphics_command_chunk_bytes_max = frame_chunk_bytes_max;
inline constexpr std::size_t graphics_decoded_image_bytes_max = std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t graphics_image_pixels_max = std::size_t{4'096} * 4'096U;
inline constexpr std::size_t graphics_placements_per_pane_max = 4'096;
inline constexpr std::size_t graphics_session_bytes_max = std::size_t{256} * 1'024U * 1'024U;

inline constexpr std::size_t terminal_pty_response_bytes_max = std::size_t{64} * 1'024U;
// One pane queue can retain the terminal adapter's complete response bound plus either one
// maximally expanded 8 KiB legacy packet or one bounded typed paste. Queue storage is allocated
// lazily under this daemon-wide budget, while accepted-input delivery remains all-or-nothing.
inline constexpr std::size_t normalized_client_input_bytes_max = std::size_t{8} * 1'024U * 128U;
inline constexpr std::size_t pane_pty_input_backlog_bytes_max =
    terminal_pty_response_bytes_max + normalized_client_input_bytes_max;
// Clipboard reply headroom must not increase ordinary input admission or its allocation ceiling.
inline constexpr std::size_t pane_pty_write_queue_bytes_max =
    pane_pty_input_backlog_bytes_max + clipboard_response_bytes_max;
inline constexpr std::size_t pane_pty_write_queue_bytes_aggregate_max =
    std::size_t{128} * 1'024U * 1'024U;
static_assert(pane_pty_write_queue_bytes_aggregate_max >= pane_pty_write_queue_bytes_max);
inline constexpr std::size_t pending_connections_hard_max = 128;
inline constexpr std::size_t pending_connection_output_bytes_max = std::size_t{64} * 1'024U;
inline constexpr std::size_t terminal_allocation_bytes_default = std::size_t{64} * 1'024U * 1'024U;
inline constexpr std::size_t terminal_allocation_bytes_hard_max =
    std::size_t{1} * 1'024U * 1'024U * 1'024U;
inline constexpr std::uint16_t terminal_columns_hard_max = 1'000;
inline constexpr std::uint16_t terminal_rows_hard_max = 1'000;
// Ghostty's PagePool allocator bypasses QuotaAllocator, so keep both page-granular scrollback
// dimensions independently bounded at the adapter boundary. The byte default matches the pinned
// Ghostty surface default and is lazy; Runtime separately reserves configured capacity against the
// aggregate bound before publishing a pane.
inline constexpr std::size_t terminal_scrollback_bytes_default = 50'000'000;
inline constexpr std::size_t terminal_scrollback_bytes_hard_max = 50'000'000;
// Reserve enough configured capacity for one maximally populated session without allowing the
// daemon-wide pane maximum to multiply the per-pane limit into hundreds of gigabytes.
inline constexpr std::size_t terminal_scrollback_bytes_aggregate_max =
    static_cast<std::size_t>(panes_hard_max / sessions_hard_max) *
    terminal_scrollback_bytes_default;
// Byte and physical-line limits are independent Ghostty pruning dimensions. A null line limit
// leaves byte accounting authoritative; callers may set both when they need a row-count ceiling.
inline constexpr std::size_t terminal_scrollback_lines_hard_max = 1'000'000;
inline constexpr std::size_t selection_format_bytes_max = std::size_t{1} * 1'024U * 1'024U;
inline constexpr std::size_t search_query_bytes_max = 256;
inline constexpr std::size_t search_candidates_per_step = 256;
inline constexpr auto scrollback_compression_idle_delay = std::chrono::seconds{1};
static_assert(terminal_scrollback_bytes_default <= terminal_scrollback_bytes_hard_max);
static_assert(terminal_scrollback_bytes_aggregate_max >= terminal_scrollback_bytes_hard_max);

} // namespace lemma::limits

#endif // LEMMA_LIMITS_HPP
