import unittest
from pathlib import Path
from rf3_gui.protocol.console_protocol import transfer_extension,upload_extension,import_filename,valid_storage_name


class TestFileTypeNames(unittest.TestCase):
    def test_normalized_long_and_unicode_names_keep_full_supported_suffix(self):
        for name in ('long report with spaces.ABCDEFGHI','résumé.PDF','x'*100+'.123456789','archive.tar.gz'):
            suffix=transfer_extension(name)
            normalized=import_filename(name)
            self.assertTrue(valid_storage_name(normalized))
            self.assertEqual(Path(normalized).suffix,suffix)
            self.assertLessEqual(len(normalized.encode('ascii')),30)

    def test_invalid_types_reject_before_normalization(self):
        for name in ('file.abcdefghij','file.PART','file.c++','file.p df','file.p"df'):
            with self.assertRaises(ValueError,msg=name):transfer_extension(name)
        self.assertEqual(transfer_extension('extensionless'),'')
        self.assertEqual(transfer_extension('image.jpeg'),'.jpeg')
        self.assertEqual(transfer_extension('archive.tar.gz'),'.gz')

    def test_host_only_suffix_can_exceed_rf_metadata_limit(self):
        name='résumé.abcdefghijkl'
        self.assertEqual(upload_extension(name),'.abcdefghijkl')
        self.assertTrue(import_filename(name).endswith('.abcdefghijkl'))
        with self.assertRaises(ValueError):transfer_extension(name)


if __name__=='__main__':unittest.main()
