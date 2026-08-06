pub enum Option {
    None,
    Some(u8),
}

pub fn identity(value: Option) -> Option {
    value
}
