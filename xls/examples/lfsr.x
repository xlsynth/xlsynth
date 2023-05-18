////////////////////////////////////////////////////////////////////////////////
// Linear-feedback shift register (LFSR)
// A parametric function to compute the next value of an LFSR counter.
// Taps can be specified to have varying periods for a given number of bits.
//
// For example, lfsr(u5:4, u5:0b10100) computes the value that comes after 4
// in a 5-bit LFSR with taps on bits 2 and 4.
////////////////////////////////////////////////////////////////////////////////

fn lfsr<BIT_WIDTH: u32>(current_value: uN[BIT_WIDTH], tap_mask: uN[BIT_WIDTH]) -> uN[BIT_WIDTH] {
    // Compute the new bit from the taps
    let new_bit = for (index, xor_bit): (u32, u1) in range(u32:0, BIT_WIDTH) {
        if tap_mask[index+:u1] == u1:0 {
            xor_bit
        } else {
            xor_bit ^ current_value[index+:u1]
        }
    } (u1:0);

    // Kick the high bit and insert the new bit
    current_value[u32:0 +: uN[BIT_WIDTH - u32:1]] ++ new_bit
}

////////////////////////////////////////////////////////////////////////////////
// Here are a few maximal LFSRs for different bit widths.
// These are only examples and it is possible to use different bit widths and
// tap masks.
// Source: https://en.wikipedia.org/wiki/Linear-feedback_shift_register
////////////////////////////////////////////////////////////////////////////////

// 7-bit LFSR with the maximal period of 127
fn lfsr7(n: u7) -> u7 {
    lfsr(n, u7:0b1100000)
}

// 8-bit LFSR with the maximal period of 255
fn lfsr8(n: u8) -> u8 {
    lfsr(n, u8:0b10111000)
}

////////////////////////////////////////////////////////////////////////////////
// Tests
////////////////////////////////////////////////////////////////////////////////

#[test]
fn lfsr7_test() {
    // sanity test
    let _ = assert_eq(lfsr7(u7:0), u7:0);

    // test a few values
    let _ = assert_eq(lfsr7(u7:1), u7:2);
    let _ = assert_eq(lfsr7(u7:104), u7:80);
    let _ = assert_eq(lfsr7(u7:31), u7:62);
    let _ = assert_eq(lfsr7(u7:67), u7:7);
    let _ = assert_eq(lfsr7(u7:9), u7:18);
    let _ = assert_eq(lfsr7(u7:107), u7:86);
    let _ = assert_eq(lfsr7(u7:108), u7:88);
    let _ = assert_eq(lfsr7(u7:88), u7:49);

    // test that the cycle works
    let _ = assert_eq(u7:1, for (_, value) in u32:0..u32:127 {
        lfsr7(value)
    } (u7:1));

    ()
}

#[test]
fn lfsr8_test() {
    // sanity test
    let _ = assert_eq(lfsr8(u8:0), u8:0);

    // test a few values
    let _ = assert_eq(lfsr8(u8:1), u8:2);
    let _ = assert_eq(lfsr8(u8:37), u8:75);
    let _ = assert_eq(lfsr8(u8:6), u8:12);
    let _ = assert_eq(lfsr8(u8:155), u8:55);
    let _ = assert_eq(lfsr8(u8:10), u8:21);
    let _ = assert_eq(lfsr8(u8:214), u8:172);
    let _ = assert_eq(lfsr8(u8:176), u8:97);
    let _ = assert_eq(lfsr8(u8:237), u8:219);

    // test that the cycle works
    let _ = assert_eq(u8:1, for (_, value) in u32:0..u32:255 {
        lfsr8(value)
    } (u8:1));

    ()
}

