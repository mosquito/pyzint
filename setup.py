from setuptools import Extension, setup

setup(
    name="pyzint",
    version="0.0.1",
    description="Python ZINT bindings",
    long_description=open("README.rst").read(),
    packages=["pyzint"],
    package_data={"pyzint": ["pyzint/pyzint.pyi"], },
    ext_modules=[
        Extension(
            "pyzint.pyzint", [
                "pyzint/pyzint.c",
                "pyzint/src/zint/backend/mailmark.c",
                "pyzint/src/zint/backend/hanxin.c",
                "pyzint/src/zint/backend/common.c",
                "pyzint/src/zint/backend/pdf417.c",
                "pyzint/src/zint/backend/pcx.c",
                "pyzint/src/zint/backend/aztec.c",
                "pyzint/src/zint/backend/ps.c",
                "pyzint/src/zint/backend/large.c",
                "pyzint/src/zint/backend/general_field.c",
                "pyzint/src/zint/backend/reedsol.c",
                "pyzint/src/zint/backend/vector.c",
                "pyzint/src/zint/backend/upcean.c",
                "pyzint/src/zint/backend/plessey.c",
                "pyzint/src/zint/backend/eci.c",
                "pyzint/src/zint/backend/rss.c",
                "pyzint/src/zint/backend/dotcode.c",
                "pyzint/src/zint/backend/gs1.c",
                "pyzint/src/zint/backend/imail.c",
                "pyzint/src/zint/backend/2of5.c",
                "pyzint/src/zint/backend/medical.c",
                "pyzint/src/zint/backend/ultra.c",
                "pyzint/src/zint/backend/postal.c",
                "pyzint/src/zint/backend/auspost.c",
                "pyzint/src/zint/backend/composite.c",
                "pyzint/src/zint/backend/code49.c",
                "pyzint/src/zint/backend/gb2312.c",
                "pyzint/src/zint/backend/library.c",
                "pyzint/src/zint/backend/code1.c",
                "pyzint/src/zint/backend/code128.c",
                "pyzint/src/zint/backend/qr.c",
                "pyzint/src/zint/backend/sjis.c",
                "pyzint/src/zint/backend/bmp.c",
                "pyzint/src/zint/backend/codablock.c",
                "pyzint/src/zint/backend/emf.c",
                "pyzint/src/zint/backend/code.c",
                "pyzint/src/zint/backend/code16k.c",
                "pyzint/src/zint/backend/gif.c",
                "pyzint/src/zint/backend/tif.c",
                "pyzint/src/zint/backend/dmatrix.c",
                "pyzint/src/zint/backend/maxicode.c",
                "pyzint/src/zint/backend/telepen.c",
                "pyzint/src/zint/backend/png.c",
                "pyzint/src/zint/backend/svg.c",
                "pyzint/src/zint/backend/gridmtx.c",
                "pyzint/src/zint/backend/dllversion.c",
                "pyzint/src/zint/backend/raster.c",
            ],
            extra_compile_args=["-g"],
            include_dirs=["pyzint/src"],
            define_macros=[("NO_PNG", "1")]
        ),
    ],
)
