import float32;

type F32 = float32::F32;

fn add4(w: F32, x: F32, y: F32, z: F32) -> F32 {
    let a = float32::add(w, x);
    let b = float32::add(y, z);
    float32::add(a, b)
}

fn add8(inputs: F32[8]) -> F32 {
    float32::add(add4_array(array_slice(inputs, 0, 4)), add4_array(array_slice(inputs, 4, 8)))
}
