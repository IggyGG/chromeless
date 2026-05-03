/* tslint:disable */
/* eslint-disable */

/**
 * Attach event listeners to `target_id` and forward encoded events
 * over a WebSocket dialed at `ws_url`.
 *
 * `target_id` MUST refer to an `<canvas>` element in the document —
 * the encoder's `web::from_mouse_event` / `web::from_wheel_event`
 * helpers take a `&HtmlCanvasElement` so they can apply CSS-px →
 * CDP-px scaling. For the fixture page, sizing the canvas 1:1
 * (`canvas.width = canvas.clientWidth; canvas.height = canvas.clientHeight`)
 * makes the scaling a no-op and the captured x/y match the dispatched
 * CDP coordinates byte-for-byte.
 *
 * Calling this function a second time replaces the previous WS and
 * reattaches listeners. Listeners from the previous init are NOT
 * detached (the fixture is single-page; reload to fully reset).
 */
export function init_test_client(target_id: string, ws_url: string): void;

export type InitInput = RequestInfo | URL | Response | BufferSource | WebAssembly.Module;

export interface InitOutput {
    readonly memory: WebAssembly.Memory;
    readonly init_test_client: (a: number, b: number, c: number, d: number) => [number, number];
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad: (a: number, b: number, c: any) => void;
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad_1: (a: number, b: number, c: any) => void;
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad_2: (a: number, b: number, c: any) => void;
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad_3: (a: number, b: number, c: any) => void;
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad_4: (a: number, b: number, c: any) => void;
    readonly wasm_bindgen__convert__closures_____invoke__h33da389654c559ad_5: (a: number, b: number, c: any) => void;
    readonly __wbindgen_exn_store: (a: number) => void;
    readonly __externref_table_alloc: () => number;
    readonly __wbindgen_externrefs: WebAssembly.Table;
    readonly __wbindgen_malloc: (a: number, b: number) => number;
    readonly __wbindgen_realloc: (a: number, b: number, c: number, d: number) => number;
    readonly __wbindgen_destroy_closure: (a: number, b: number) => void;
    readonly __externref_table_dealloc: (a: number) => void;
    readonly __wbindgen_start: () => void;
}

export type SyncInitInput = BufferSource | WebAssembly.Module;

/**
 * Instantiates the given `module`, which can either be bytes or
 * a precompiled `WebAssembly.Module`.
 *
 * @param {{ module: SyncInitInput }} module - Passing `SyncInitInput` directly is deprecated.
 *
 * @returns {InitOutput}
 */
export function initSync(module: { module: SyncInitInput } | SyncInitInput): InitOutput;

/**
 * If `module_or_path` is {RequestInfo} or {URL}, makes a request and
 * for everything else, calls `WebAssembly.instantiate` directly.
 *
 * @param {{ module_or_path: InitInput | Promise<InitInput> }} module_or_path - Passing `InitInput` directly is deprecated.
 *
 * @returns {Promise<InitOutput>}
 */
export default function __wbg_init (module_or_path?: { module_or_path: InitInput | Promise<InitInput> } | InitInput | Promise<InitInput>): Promise<InitOutput>;
