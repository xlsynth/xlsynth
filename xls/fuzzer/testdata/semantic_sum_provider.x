pub enum Option {
    None,
    Some(u8),
}

pub enum Tag : u2 {
    Zero = 0,
    One = 1,
    Two = 2,
    Three = 3,
}

pub fn identity(value: Option) -> Option {
    value
}
