/**
 * Karl Eido - Triangular Grid Mirror Reflection App
 * 
 * Displays mirror axis and reflections on a triangular grid of equilateral triangles.
 * Controls:
 * - Up/Down: Adjust triangle size (5-63px in 2px steps)
 * - Left/Right: Decrease/increase random pixels in base triangle
 * - OK (short press): Toggle center point display
 * - Back: Exit app
 */

#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdlib.h>

// Screen dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// Triangle size constraints
#define MIN_SIDE_LENGTH 5
#define MAX_SIDE_LENGTH 63
#define SIDE_LENGTH_STEP 2
#define CENTER_Y 31

// Dash-dot pattern: ". .. " = 1px black, 1px white, 2px black, 1px white
#define DASH_DOT_PATTERN_LENGTH 5
static const uint8_t DASH_DOT_PATTERN[DASH_DOT_PATTERN_LENGTH] = {1, 0, 1, 1, 0};

// Maximum number of random pixels to store
#define MAX_PIXELS 200

// Point structure for coordinates
typedef struct {
    int x;
    int y;
} Point;

// Application state
typedef struct {
    int side_length;
    int num_random_pixels;
    bool show_centers;
    bool running;
    Point* pixel_buffer;  // Heap-allocated pixel buffer
    int pixel_count;
} AppState;

/**
 * Calculate the height of an equilateral triangle given its side length
 * Height = side_length * sqrt(3)/2
 */
static float triangle_height(int side_length) {
    return (float)side_length * 0.866025404f; // sqrt(3)/2
}

/**
 * Draw a dash-dotted line between two points using Bresenham's algorithm
 * The pattern is defined by DASH_DOT_PATTERN constant
 */
static void canvas_draw_line_dash_dot(Canvas* canvas, int x1, int y1, int x2, int y2) {
    int dx = abs(x2 - x1);
    int dy = abs(y2 - y1);
    int sx = (x1 < x2) ? 1 : -1;
    int sy = (y1 < y2) ? 1 : -1;
    int err = dx - dy;
    
    int pattern_index = 0;
    int x = x1;
    int y = y1;
    
    while(true) {
        // Draw pixel if pattern says so (1 = black/draw, 0 = white/skip)
        if(DASH_DOT_PATTERN[pattern_index]) {
            canvas_draw_dot(canvas, x, y);
        }
        
        // Move to next pattern element (cyclic)
        pattern_index = (pattern_index + 1) % DASH_DOT_PATTERN_LENGTH;
        
        // Check if we've reached the end point
        if(x == x2 && y == y2) break;
        
        // Bresenham's line algorithm step
        int e2 = 2 * err;
        if(e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if(e2 < dx) {
            err += dx;
            y += sy;
        }
    }
}

/**
 * Check if a point is inside a triangle using barycentric coordinates
 */
static bool point_in_triangle(int px, int py, Point* vertices) {
    int x0 = vertices[0].x, y0 = vertices[0].y;
    int x1 = vertices[1].x, y1 = vertices[1].y;
    int x2 = vertices[2].x, y2 = vertices[2].y;
    
    int denom = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if(denom == 0) return false;
    
    float a = ((y1 - y2) * (px - x2) + (x2 - x1) * (py - y2)) / (float)denom;
    float b = ((y2 - y0) * (px - x2) + (x0 - x2) * (py - y2)) / (float)denom;
    float c = 1.0f - a - b;
    
    return a >= 0 && a <= 1 && b >= 0 && b <= 1 && c >= 0 && c <= 1;
}

/**
 * Calculate triangle vertices based on grid position and orientation
 * 
 * @param vertices Output array for 3 vertices
 * @param col Column in the grid
 * @param row Row in the grid
 * @param side_length Side length of the triangle
 * @param pointing_right True if triangle points right, false if points left
 */
static void get_triangle_vertices(
    Point* vertices,
    int col,
    int row,
    int side_length,
    bool pointing_right) {
    
    float h = triangle_height(side_length);
    int base_x = (int)(col * h);
    int base_y = CENTER_Y + (row * side_length / 2);
    
    if(pointing_right) {
        // Triangle pointing right: |>
        vertices[0].x = base_x;
        vertices[0].y = base_y - side_length / 2;
        vertices[1].x = base_x;
        vertices[1].y = base_y + side_length / 2;
        vertices[2].x = base_x + (int)h;
        vertices[2].y = base_y;
    } else {
        // Triangle pointing left: <|
        vertices[0].x = base_x;
        vertices[0].y = base_y;
        vertices[1].x = base_x + (int)h;
        vertices[1].y = base_y - side_length / 2;
        vertices[2].x = base_x + (int)h;
        vertices[2].y = base_y + side_length / 2;
    }
}

/**
 * Calculate the center (centroid) of a triangle
 */
static Point get_triangle_center(Point* vertices) {
    Point center;
    center.x = (vertices[0].x + vertices[1].x + vertices[2].x) / 3;
    center.y = (vertices[0].y + vertices[1].y + vertices[2].y) / 3;
    return center;
}

/**
 * Draw the triangle outline with dash-dotted lines
 */
static void draw_triangle_outline(Canvas* canvas, Point* vertices) {
    canvas_draw_line_dash_dot(canvas, vertices[0].x, vertices[0].y, vertices[1].x, vertices[1].y);
    canvas_draw_line_dash_dot(canvas, vertices[1].x, vertices[1].y, vertices[2].x, vertices[2].y);
    canvas_draw_line_dash_dot(canvas, vertices[2].x, vertices[2].y, vertices[0].x, vertices[0].y);
}

/**
 * Check if a triangle is visible on screen
 */
static bool is_triangle_visible(Point* vertices) {
    // Check horizontal bounds
    if(vertices[2].x < 0 || vertices[0].x > SCREEN_WIDTH) return false;
    
    // Check vertical bounds
    if(vertices[0].y > SCREEN_HEIGHT && 
       vertices[1].y > SCREEN_HEIGHT && 
       vertices[2].y > SCREEN_HEIGHT) return false;
       
    if(vertices[0].y < 0 && vertices[1].y < 0 && vertices[2].y < 0) return false;
    
    return true;
}

/**
 * Generate random pixels in the base triangle
 */
static void generate_random_pixels(AppState* state) {
    if(state == NULL || state->pixel_buffer == NULL) return;
    if(state->side_length < MIN_SIDE_LENGTH) return;
    
    float h = triangle_height(state->side_length);
    
    // Get base triangle (column 0, row 0, pointing right)
    Point base_vertices[3];
    get_triangle_vertices(base_vertices, 0, 0, state->side_length, true);
    Point base_center = get_triangle_center(base_vertices);
    
    // Generate random pixels in base triangle
    state->pixel_count = 0;
    
    for(int i = 0; i < state->num_random_pixels && state->pixel_count < MAX_PIXELS; i++) {
        int px = base_vertices[0].x + (rand() % ((int)h + 1));
        int py = base_vertices[0].y - state->side_length / 2 + (rand() % (state->side_length + 1));
        
        if(point_in_triangle(px, py, base_vertices)) {
            // Skip if pixel would be at center point
            if(px == base_center.x && py == base_center.y) continue;
            
            state->pixel_buffer[state->pixel_count].x = px;
            state->pixel_buffer[state->pixel_count].y = py;
            state->pixel_count++;
        }
    }
}

/**
 * Draw the pattern using pre-generated pixels
 */
static void draw_pattern(Canvas* canvas, AppState* state) {
    if(state == NULL || canvas == NULL) return;
    if(state->side_length < MIN_SIDE_LENGTH) return;
    
    float h = triangle_height(state->side_length);
    
    // Get base triangle for reference
    Point base_vertices[3];
    get_triangle_vertices(base_vertices, 0, 0, state->side_length, true);
    Point base_center = get_triangle_center(base_vertices);
    
    // Calculate grid dimensions
    int num_cols = (int)(SCREEN_WIDTH / h) + 2;
    int num_rows = (int)(SCREEN_HEIGHT / (state->side_length / 2.0f)) + 2;
    
    // Statistics for debug display
    int total_triangles = 0;
    int total_area = 0;
    
    // Draw all triangles in the grid
    for(int col = 0; col < num_cols; col++) {
        for(int row = -num_rows; row < num_rows; row++) {
            // Alternate between pointing right and left based on grid position
            bool pointing_right = ((col + row) % 2 == 0);
            
            Point vertices[3];
            get_triangle_vertices(vertices, col, row, state->side_length, pointing_right);
            
            // Skip if triangle is not visible
            if(!is_triangle_visible(vertices)) continue;
            
            // Draw triangle outline
            draw_triangle_outline(canvas, vertices);
            
            // Mirror and draw pixels from base triangle
            Point curr_center = get_triangle_center(vertices);
            int area = 0;
            
            for(int p = 0; p < state->pixel_count; p++) {
                int px = state->pixel_buffer[p].x;
                int py = state->pixel_buffer[p].y;
                
                // Mirror pixel relative to triangle centers
                int mirrored_x = curr_center.x + (px - base_center.x);
                int mirrored_y = curr_center.y + (py - base_center.y);
                
                // Draw if within screen bounds
                if(mirrored_x >= 0 && mirrored_x < SCREEN_WIDTH && 
                   mirrored_y >= 0 && mirrored_y < SCREEN_HEIGHT) {
                    canvas_draw_dot(canvas, mirrored_x, mirrored_y);
                    area++;
                }
            }
            
            // Draw center point if enabled
            if(state->show_centers && 
               curr_center.x >= 0 && curr_center.x < SCREEN_WIDTH && 
               curr_center.y >= 0 && curr_center.y < SCREEN_HEIGHT) {
                canvas_draw_disc(canvas, curr_center.x, curr_center.y, 1);
                total_triangles++;
            }
            
            total_area += area;
        }
    }
    
    // Draw debug info: "# [avg area] T: [visible centers]"
    char debug_str[32];
    int avg_area = (total_triangles > 0) ? (total_area / total_triangles) : 0;
    snprintf(debug_str, sizeof(debug_str), "# %d T: %d", avg_area, total_triangles);
    
    // Draw white background for text
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, SCREEN_WIDTH - 60, 0, 60, 10);
    
    // Draw text in black
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_str(canvas, SCREEN_WIDTH - 58, 8, debug_str);
}

/**
 * Canvas render callback
 */
static void render_callback(Canvas* canvas, void* ctx) {
    AppState* state = (AppState*)ctx;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    
    draw_pattern(canvas, state);
}

/**
 * Input event callback
 */
static void input_callback(InputEvent* input_event, void* ctx) {
    furi_assert(ctx);
    FuriMessageQueue* event_queue = ctx;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

/**
 * Handle input events and update application state
 */
static bool handle_input(InputEvent* event, AppState* state) {
    if(event->type != InputTypePress && event->type != InputTypeRepeat) {
        return false;
    }
    
    bool state_changed = false;
    bool regenerate_pixels = false;
    
    switch(event->key) {
        case InputKeyUp:
            if(state->side_length < MAX_SIDE_LENGTH) {
                state->side_length += SIDE_LENGTH_STEP;
                state_changed = true;
                regenerate_pixels = true;
            }
            break;
            
        case InputKeyDown:
            if(state->side_length > MIN_SIDE_LENGTH) {
                state->side_length -= SIDE_LENGTH_STEP;
                state_changed = true;
                regenerate_pixels = true;
            }
            break;
            
        case InputKeyLeft:
            if(state->num_random_pixels > 0) {
                state->num_random_pixels--;
                state_changed = true;
                regenerate_pixels = true;
            }
            break;
            
        case InputKeyRight:
            state->num_random_pixels++;
            state_changed = true;
            regenerate_pixels = true;
            break;
            
        case InputKeyOk:
            if(event->type == InputTypePress) {
                state->show_centers = !state->show_centers;
                state_changed = true;
            }
            break;
            
        case InputKeyBack:
            state->running = false;
            break;
            
        default:
            break;
    }
    
    // Regenerate pixels if needed
    if(regenerate_pixels) {
        generate_random_pixels(state);
    }
    
    return state_changed;
}

/**
 * Main application entry point
 * Entry point name must match application.fam: entry_point="karl_main"
 */
int32_t karl_main(void* p) {
    UNUSED(p);
    
    // Initialize application state
    AppState* state = malloc(sizeof(AppState));
    if(state == NULL) {
        return -1; // Failed to allocate memory
    }
    
    // Allocate pixel buffer on heap
    state->pixel_buffer = malloc(sizeof(Point) * MAX_PIXELS);
    if(state->pixel_buffer == NULL) {
        free(state);
        return -1;
    }
    
    state->side_length = MIN_SIDE_LENGTH;
    state->num_random_pixels = 0;
    state->pixel_count = 0;
    state->show_centers = false;
    state->running = true;
    
    // Generate initial (empty) pixel set
    generate_random_pixels(state);
    
    // Create event queue
    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    if(event_queue == NULL) {
        free(state->pixel_buffer);
        free(state);
        return -1;
    }
    
    // Set up viewport
    ViewPort* view_port = view_port_alloc();
    if(view_port == NULL) {
        furi_message_queue_free(event_queue);
        free(state->pixel_buffer);
        free(state);
        return -1;
    }
    
    view_port_draw_callback_set(view_port, render_callback, state);
    view_port_input_callback_set(view_port, input_callback, event_queue);
    
    // Register viewport with GUI
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    
    // Main event loop
    InputEvent event;
    while(state->running) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            if(handle_input(&event, state)) {
                view_port_update(view_port);
            }
        }
    }
    
    // Cleanup
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_record_close(RECORD_GUI);
    free(state->pixel_buffer);
    free(state);
    
    return 0;
}