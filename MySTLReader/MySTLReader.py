import struct


def read_zero_xyz(path):
    with open(path, "rb") as f:
        data = f.read(4 + 60 + 4 + 4 + 4 + 4)

    if len(data) < 76:
        raise ValueError("Файл слишком маленький")

    fmt = "<4s60s3f4s"
    stl_tag, unused, zx, zy, zz, unused2 = struct.unpack(fmt, data)

    print("STL tag:", stl_tag.decode(errors="ignore"))
    print("zero_x:", zx)
    print("zero_y:", zy)
    print("zero_z:", zz)


if __name__ == "__main__":
    path0 = r"D:\CT36\.25 Kneazev\CT\chamber0\tors.stl"
    read_zero_xyz(path0)
    path1 = r"D:\CT36\.25 Kneazev\CT\chamber0\heart0.stl"
    read_zero_xyz(path1)
