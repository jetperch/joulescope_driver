declare module 'joulescope_driver' {

    export interface Value {
        data: number[]
        decimate_factor: number
        field_id: number
        index: number
        sample_id: number
        sample_rate: number
        time_map: {
            offset_time: number
            offset_counter: number
            counter_rate: number
        }
        utc: number
    }

    class JoulescopeDriver {
        constructor()

        publish(topic: string, value: any, timeout?: number): void

        query(topic: string, timeout?: number): any

        finalize(): void

        device_paths(timeout?: number): string[]

        open(topic: string, mode?: number): void

        close(topic: string): void

        subscribe(
            topic: string,
            flags: number,
            fn: (topic: string, value: Value) => void,
            timeout?: number
        ): () => void
    }

    export default JoulescopeDriver
}
